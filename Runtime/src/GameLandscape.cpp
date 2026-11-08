#include "aver/game/GameLandscape.hpp"
#include "aver/core/Types.hpp"   // AVER_PATH_SEP

#if AVER_MODULE_LANDSCAPE

#include "aver/game/GameContent.hpp"
#include "aver/game/PhysicalSurfaces.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/SurfaceId.hpp"
#include "aver/landscape/HeightfieldRay.hpp"
#include "aver/landscape/PhysicsBridge.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <vector>

#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#  include "aver/physics/physics_shape_builder_abi.h"
#  include "aver/physics/physics_terrain_abi.h"
#  include "aver/physics/physics_water_abi.h"
#endif
#if AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_terrain_abi.h"
#endif

namespace aver::game {

bool GameLandscape::loadLandscape(rhi::IDevice* device, const std::string& path) {
    unload(device);
    fmt::OcLandData data;
    std::string why;
    if (!fmt::loadOcLand(path, data, &why)) {
        AVER_WARN("[Landscape] could not load '{}': {}", path, why);
        return false;
    }
    // Floating origin: the file's origin is absolute; the section lives in the current frame (a no-op at origin zero).
    homeAbs_[0] = static_cast<f64>(data.originCm[0]);
    homeAbs_[1] = static_cast<f64>(data.originCm[1]);
    placeInFrame(data);
    landscape::LandscapeTree tree;
    if (!tree.build(data, landscape::kDefaultNodeQuads, &why)) {
        AVER_WARN("[Landscape] '{}' loaded but its quadtree would not build: {}", path, why);
        return false;
    }
    landscapeData_ = std::move(data);
    landscapeTree_ = std::move(tree);
    if (!landscapeData_.warning.empty())
        AVER_WARN("[Landscape] '{}': the stored paint layers were dropped: {}", path, landscapeData_.warning);
    rebuildRemap();   // wave3 layers
    // The section is tile (0,0) of the ring now -- its outer rim borders a procedural neighbour like
    // any ring tile's does, needing the same generous floor. 2x the noise amplitude is the
    // mathematical bound on how much a ridged-fBm field can vary at all (TerrainNoise.hpp).
    landscapeTree_.widenRimSkirts(2.0f * landscapeNoiseParams_.amplitudeCm);
    landscapeTree_.resetHysteresis();
    landscapeRenderer_ = std::make_unique<landscape::LandscapeRenderer>(kMaxResidentNodesPerTile);
    landscapeRenderer_->setOrigin(frame_.x, frame_.y);
    loaded_ = true;
    landscapePath_ = path;
    dirty_ = false;
    // NEW HEIGHTS MEAN THE OLD SHAPE IS WRONG, and for one caller there is no shape at all.
    //
    // loadForLevel calls rebuildCollision() immediately after this returns, so for that path this
    // flag is set and cleared within a few lines and changes nothing. The caller it exists for is
    // loadSection() -- the public form, whose own comment says plainly that it does NO collision
    // rebuild and that "a caller that needs those calls them itself afterward". Its single caller is
    // Create Landscape (sandbox/src/SandboxLevelEdit.cpp:52), and it does not call them afterward.
    //
    // So a freshly created landscape had no collision body at ALL -- not a stale one, none -- and
    // the first thing anyone does with a new terrain is press Play and walk on it. Marking it here
    // rather than at that call site is what makes the guarantee survive the next caller of
    // loadSection, who will be just as unlikely to read that comment.
    collisionStale_ = true;
    AVER_INFO("[Landscape] '{}' loaded: {} node(s) across {} level(s), {}x{} samples",
              path, landscapeTree_.nodes().size(), landscapeTree_.levelCount(),
              landscapeData_.sampleCount, landscapeData_.sampleCount);
    return true;
}

i32 GameLandscape::buildBody(const fmt::OcLandData& d, const std::array<i32, 8>& remap) const {
#if AVER_MODULE_PHYSICS
    // Each layer's physical surface, named for every section layer, so the heightfield can carry one surface per cell
    // (the dominant paint layer).
    std::vector<i32> layerSurfaces;
    if (!layerTable_.empty()) {
        constexpr usize kSectionLayerCap = 8;   // layersValid(): at most 8 section layers
        const usize sectionLayers = std::min<usize>(std::max<usize>(d.layerNames.size(), 1), kSectionLayerCap);
        for (usize s = 0; s < sectionLayers; ++s) {
            // The same remap sampleGround reads: a section layer the material does not know folds into its base layer.
            const i32 c = s < d.layerNames.size() ? remap[s] : 0;
            layerSurfaces.push_back(aver::surfaceIdFromName(layerTable_[c >= 0 ? static_cast<usize>(c) : 0].surfaceName()));
        }
    }
    landscape::PhysicsHeightfield hf;
    if (!landscape::toPhysicsHeightfield(d, hf, layerSurfaces.empty() ? nullptr : &layerSurfaces)) {
        AVER_WARN("[Landscape] section is not internally consistent; no collision built");
        return -1;
    }
    i32 body = 0;
    if (hf.cellPalette.empty()) {
        body = aver_phys_add_heightfield(hf.samples.data(), static_cast<i32>(hf.sampleCount),
                                         hf.spacingCm, hf.cornerCm[0], hf.cornerCm[1], hf.cornerCm[2]);
    } else {
        // The shape route, with a surface palette and one palette index per cell.
        const i32 shape = aver_phys_heightfield_shape(hf.samples.data(), static_cast<i32>(hf.sampleCount), static_cast<i32>(hf.sampleCount),
                                                      hf.spacingCm, hf.cellPalette.data(), hf.palette.data(),
                                                      static_cast<i32>(hf.palette.size()), nullptr, 0);
        if (shape) {
            body = aver_phys_body_from_shape(shape, hf.cornerCm, nullptr, AVER_PHYS_MOTION_STATIC, 0.0f, nullptr, 0);
            aver_phys_shape_release(shape);   // the body keeps the shape alive
        }
    }
    // Deliberately NOT stamped with aver_phys_set_entity: terrain has no owning scene entity in this
    // engine at all. A ray landing on it is a genuine "hit true, entity 0", not a bug.
    //
    // A REFUSED BUILD RETURNS 0, NOT A NEGATIVE: handles start at 1.
    if (body > 0) {
        AVER_INFO("[Landscape] collision body #{} built ({}x{} samples{})", body,
                  hf.sampleCount, hf.sampleCount, hf.cellPalette.empty() ? "" : ", a surface per cell");
        return body;
    }
    AVER_WARN("[Landscape] physics refused the heightfield; terrain has no collision");
#else
    (void)d; (void)remap;
#endif
    return -1;
}

void GameLandscape::rebuildCollision() {
#if AVER_MODULE_PHYSICS
    if (landscapeBody_ >= 0) { aver_phys_remove_body(landscapeBody_); landscapeBody_ = -1; }
    if (!loaded_ || !aver_phys_ready()) return;
    // Registered (find-or-add, so an authored Surfaces.ocsurf keeps its numbers) before the cells name them.
    registerLayerSurfaces();
    landscapeBody_ = buildBody(landscapeData_, sectionToMaterial_);
    updateSurfaceSampler();   // wave3 layers: the new body takes the layers' friction
#endif
    // CLEARED EVEN WITHOUT PHYSICS COMPILED IN, and even when the build above failed. The flag means
    // "the shape no longer describes the mesh, so ask again at the next moment that matters", and in
    // both of those cases asking again would fail identically -- latching it true would turn one
    // refused heightfield into a rebuild attempt on every single Play for the rest of the session.
    collisionStale_ = false;
}

// The extra sections' bodies: built at level load only (they are not editable, so nothing makes them stale).
void GameLandscape::rebuildExtraCollision() {
#if AVER_MODULE_PHYSICS
    for (Section& e : extras_)
        if (e.body >= 0) { aver_phys_remove_body(e.body); e.body = -1; }
    if (!loaded_ || extras_.empty() || !aver_phys_ready()) return;
    registerLayerSurfaces();
    for (Section& e : extras_) e.body = buildBody(e.data, e.remap);
    updateSurfaceSampler();
#endif
}

void GameLandscape::rebuildCollisionIfStale() {
    // The whole point is that this is CHEAP WHEN NOTHING MOVED: the common Play, on terrain nobody
    // has touched since it loaded, costs one branch. Only an actual edit pays for a rebuild, and
    // then exactly once rather than once per stroke.
    if (!collisionStale_) return;
    AVER_INFO("[Landscape] heights changed since the collision body was built -- rebuilding so what "
              "you walk on matches what you sculpted");
    rebuildCollision();
}

void GameLandscape::unload(rhi::IDevice* device) {
    if (landscapeRenderer_ && device) landscapeRenderer_->forgetAll(*device);
    landscapeRenderer_.reset();
#if AVER_MODULE_PHYSICS
    if (landscapeBody_ >= 0) { aver_phys_remove_body(landscapeBody_); landscapeBody_ = -1; }
#endif
    for (Section& e : extras_) {
        if (e.renderer && device) e.renderer->forgetAll(*device);
#if AVER_MODULE_PHYSICS
        if (e.body >= 0) { aver_phys_remove_body(e.body); e.body = -1; }
#endif
    }
    primaryLastDrawn_ = 0;
    loaded_ = false;
    landscapePath_.clear();
    landscapeData_ = fmt::OcLandData{};
    for (auto& kv : landscapeRingTiles_)
        if (kv.second.renderer && device) kv.second.renderer->forgetAll(*device);
    landscapeRingTiles_.clear();
    lastCameraTileValid_ = false;

    dirty_ = false;
    strokeActive_ = false;
    strokeEmpty_ = true;
    strokeBefore_.clear();
    pendingInvalidate_ = false;

    // ---- wave3 layers ----
    layerTable_.clear();
    sectionToMaterial_.fill(-1);
    weightsDirty_ = false;
    wStrokeActive_ = false;
    wStrokeEmpty_ = true;
    wStrokeBefore_.clear();
#if AVER_MODULE_PHYSICS
    aver_phys_clear_surface_sampler();
#endif
#if AVER_MODULE_PBR
    releaseTerrain(device);   // before the extras go: it retires their textures
#endif
    extras_.clear();
    // ---- end wave3 layers ----
}

bool GameLandscape::findSection(f32 xCm, f32 yCm, Hit& out) const {
    if (!loaded_) return false;
    f32 z = 0.0f;
    if (landscape::surfaceHeightAt(landscapeData_, xCm, yCm, z)) {
        out = Hit{&landscapeData_, &sectionToMaterial_, z};
        return true;
    }
    for (const Section& e : extras_) {
        if (xCm < e.data.boundsMin[0] || xCm > e.data.boundsMax[0] || yCm < e.data.boundsMin[1] || yCm > e.data.boundsMax[1])
            continue;
        if (landscape::surfaceHeightAt(e.data, xCm, yCm, z)) {
            out = Hit{&e.data, &e.remap, z};
            return true;
        }
    }
    return false;
}

bool GameLandscape::groundHeightAt(f64 worldXCm, f64 worldYCm, f64& outWorldZCm) const {
    // The arguments are ABSOLUTE (level records); the sections are in the scene frame. Z is never rebased.
    Hit h;
    if (!findSection(static_cast<f32>(worldXCm - static_cast<f64>(frame_.x)),
                     static_cast<f32>(worldYCm - static_cast<f64>(frame_.y)), h))
        return false;
    outWorldZCm = static_cast<f64>(h.z);
    return true;
}

bool GameLandscape::scatterHeightAt(f32 worldXCm, f32 worldYCm, f32& outWorldZCm) const {
    Hit h;
    if (findSection(worldXCm, worldYCm, h)) { outWorldZCm = h.z; return true; }
    // Past the rim the field is a function of the ABSOLUTE position (f64 sum): the same hill at any origin.
    outWorldZCm = landscape::terrainHeightAtFrom(worldXCm, worldYCm, frame_.x, frame_.y, landscapeNoiseParams_);
    return true;
}

#if AVER_MODULE_PBR
void GameLandscape::applySurface(landscape::LandscapeRenderer& r, GameContent& content,
                                  pbr::MaterialSystem& materials, bool ring, u32 sectionMat) {
    if (landscapeMaterial_.empty()) return;
    if (!materials.ready()) return;
    const pbr::MaterialHandle h = content.materialForSurface(landscapeMaterial_);
    if (!h) return;
    // wave3 layers: a layered terrain material draws through its derived copy -- the section's carries its weightmaps,
    // the ring tiles' has none.
    pbr::MaterialHandle use = h;
    if (terrainActive_) {
        use = ring ? ringMat_ : (sectionMat ? sectionMat : secMat_);
        if (!use) use = h;
    }
    const pbr::MaterialConstants& mc = materials.constants(use);
    r.setSurfaceBinding(materials.bindingSet(use), &mc, sizeof(pbr::MaterialConstants));

    // AND THE TEXTURE SCALE -- read from the resolved material's own uvTilesPerCm, not the fallback's:
    // constants() returns fallbackConstants_ for any handle not yet valid, and materialForSurface()
    // can create one on the very frame this runs, so ask the library the same question constants()
    // asks and retry until it says yes.
    if (pbr::MaterialLibrary::get().valid(h) && mc.uvTilesPerCm > 0.0f) {
        landscapeUvTilingCm_ = 1.0f / mc.uvTilesPerCm;
        landscapeUvTilingResolved_ = true;
    }
}

void GameLandscape::applySurfaceToAll(rhi::IDevice* device, GameContent& content,
                                       pbr::MaterialSystem& materials) {
    const f32 wasTiling = landscapeUvTilingCm_;
    syncTerrain(content, materials);   // wave3 layers: derived materials first, so the bindings below pick them
    if (landscapeRenderer_) applySurface(*landscapeRenderer_, content, materials);
    // An unpainted extra is the base layer everywhere, exactly what the ring material draws.
    for (Section& e : extras_)
        if (e.renderer) applySurface(*e.renderer, content, materials, !e.data.hasLayers(), e.mat);
    for (auto& kv : landscapeRingTiles_)
        if (kv.second.renderer) applySurface(*kv.second.renderer, content, materials, true);

    // THE CACHED MESHES CARRY THE OLD SCALE, so eviction belongs here (compared once around the whole
    // sweep), not inside applySurface, which runs once per renderer and would leave every OTHER ring
    // tile holding UVs built at the previous tiling.
    if (device && landscapeUvTilingCm_ != wasTiling) {
        if (landscapeRenderer_) landscapeRenderer_->forgetAll(*device);
        for (Section& e : extras_)
            if (e.renderer) e.renderer->forgetAll(*device);
        for (auto& kv : landscapeRingTiles_)
            if (kv.second.renderer) kv.second.renderer->forgetAll(*device);
        AVER_INFO("[Landscape] texture tiling {:.0f}cm per tile, from material '{}' (was {:.0f}); "
                  "resident nodes dropped to rebuild",
                  landscapeUvTilingCm_, landscapeMaterial_, wasTiling);
    }
}
#endif // AVER_MODULE_PBR

void GameLandscape::placeSection(fmt::OcLandData& d, const f64 absXY[2], const f64* atZ) const {
    d.originCm[0] = static_cast<f32>(absXY[0] - static_cast<f64>(frame_.x));   // absolute record, current frame
    d.originCm[1] = static_cast<f32>(absXY[1] - static_cast<f64>(frame_.y));
    if (atZ) d.originCm[2] = static_cast<f32>(*atZ);
    d.boundsMin[0] = d.originCm[0];
    d.boundsMin[1] = d.originCm[1];
    d.boundsMax[0] = d.originCm[0] + d.extentCm();
    d.boundsMax[1] = d.originCm[1] + d.extentCm();
}

bool GameLandscape::loadExtra(rhi::IDevice* device, const std::string& path, bool haveAt, const f64 at[3]) {
    (void)device;
    Section sec;
    std::string why;
    if (!fmt::loadOcLand(path, sec.data, &why)) {
        AVER_WARN("[Landscape] could not load extra section '{}': {}", path, why);
        return false;
    }
    sec.homeAbs[0] = haveAt ? at[0] : static_cast<f64>(sec.data.originCm[0]);
    sec.homeAbs[1] = haveAt ? at[1] : static_cast<f64>(sec.data.originCm[1]);
    placeSection(sec.data, sec.homeAbs, haveAt ? &at[2] : nullptr);
    if (!sec.tree.build(sec.data, landscape::kDefaultNodeQuads, &why)) {
        AVER_WARN("[Landscape] extra section '{}' loaded but its quadtree would not build: {}", path, why);
        return false;
    }
    if (!sec.data.warning.empty())
        AVER_WARN("[Landscape] '{}': the stored paint layers were dropped: {}", path, sec.data.warning);
    sec.tree.widenRimSkirts(2.0f * landscapeNoiseParams_.amplitudeCm);
    sec.tree.resetHysteresis();
    sec.renderer = std::make_unique<landscape::LandscapeRenderer>(kMaxResidentNodesPerTile);
    sec.renderer->setOrigin(frame_.x, frame_.y);
    sec.path = path;
    remapFor(sec.data, sec.remap);
    AVER_INFO("[Landscape] extra section '{}': {} node(s), {}x{} samples at ({:.0f}, {:.0f})", path,
              sec.tree.nodes().size(), sec.data.sampleCount, sec.data.sampleCount, sec.homeAbs[0], sec.homeAbs[1]);
    extras_.push_back(std::move(sec));
    return true;
}

void GameLandscape::loadForLevel(rhi::IDevice* device, const std::string& contentDir,
                                  const std::string& levelPath, const fmt::OcWorldData& w,
                                  GameContent* content, pbr::MaterialSystem* materials) {
    std::string path = pathOverride_;
    // With an override the level's records are not read at all, extras included.
    const bool useRecords = path.empty();
    bool haveAt = false;
    f64 at[3] = {0, 0, 0};

    // The level's own LANDSCAPE record, if it has one -- resolved against `contentDir` and placed at
    // the record's own `at`. Skipped entirely when setPathOverride() supplied a path already.
    if (useRecords && !w.landscapes.empty()) {
        const fmt::OcLandscapePlacement& lp = w.landscapes.front();
        // Taken even when the section path below fails: the material is a property of the level's
        // terrain, not of which file the heights came from, and the .ocland fallback convention still
        // wants it.
        landscapeMaterial_ = lp.material;
        if (!lp.section.empty()) {
            path = contentDir.empty() ? lp.section : contentDir + AVER_PATH_SEP + lp.section;
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) {
                AVER_WARN("[Landscape] level's LANDSCAPE section '{}' does not exist at '{}' -- "
                          "falling back to the levelname.ocland convention", lp.section, path);
                path.clear();
            } else {
                at[0] = lp.x; at[1] = lp.y; at[2] = lp.z;
                haveAt = true;
            }
        }
    }

    const bool explicitPath = !path.empty();
    if (!explicitPath) {
        std::filesystem::path p(levelPath);
        p.replace_extension(".ocland");
        path = p.string();
    }
    std::error_code ec;
    if (!explicitPath && !std::filesystem::exists(path, ec)) return;
    if (!loadLandscape(device, path)) return;

    if (haveAt) {
        homeAbs_[0] = at[0];
        homeAbs_[1] = at[1];
        placeSection(landscapeData_, homeAbs_, &at[2]);
        // The tree caches node centres and bounds derived from originCm, so it has to be rebuilt
        // rather than nudged -- otherwise LOD selection and frustum culling would run against where
        // the section used to be.
        std::string why;
        if (landscapeTree_.build(landscapeData_, landscapeTree_.nodeQuads(), &why)) {
            landscapeTree_.widenRimSkirts(2.0f * landscapeNoiseParams_.amplitudeCm);
            landscapeTree_.resetHysteresis();
            if (landscapeRenderer_ && device) landscapeRenderer_->forgetAll(*device);
            // The section moved, so its ring-tile grid (centred on ITS centre) moved too -- whatever
            // was resident was built against the old placement. Drop the ring and let
            // updateRingTiles() resynthesize next frame.
            for (auto& kv : landscapeRingTiles_)
                if (kv.second.renderer && device) kv.second.renderer->forgetAll(*device);
            landscapeRingTiles_.clear();
            lastCameraTileValid_ = false;
            AVER_INFO("[Landscape] placed at ({:.0f}, {:.0f}, {:.0f}) by the level's LANDSCAPE record",
                      at[0], at[1], at[2]);
        } else {
            AVER_ERROR("[Landscape] could not rebuild after placement: {}", why);
        }
    }

    // Every further LANDSCAPE record is an extra section at its own `at`.
    if (useRecords) {
        for (usize i = 1; i < w.landscapes.size(); ++i) {
            const fmt::OcLandscapePlacement& lp = w.landscapes[i];
            if (lp.section.empty()) continue;
            const std::string p = contentDir.empty() ? lp.section : contentDir + AVER_PATH_SEP + lp.section;
            if (!std::filesystem::exists(p, ec)) {
                AVER_WARN("[Landscape] level's LANDSCAPE section '{}' does not exist at '{}' -- skipped", lp.section, p);
                continue;
            }
            const f64 pos[3] = {lp.x, lp.y, lp.z};
            loadExtra(device, p, true, pos);
        }
    }
    rebuildCollision();
    rebuildExtraCollision();
    if (terrainChanged_) terrainChanged_();
#if AVER_MODULE_PBR
    if (content && materials) applySurfaceToAll(device, *content, *materials);
#else
    (void)content; (void)materials;
#endif
}

void GameLandscape::updateRingTiles(rhi::IDevice* device, f32 cameraXCm, f32 cameraYCm,
                                     GameContent* content, pbr::MaterialSystem* materials) {
#if !AVER_MODULE_PBR
    (void)content; (void)materials;
#endif
    if (!loaded_) return;
    const f32 tileSizeCm = landscapeData_.extentCm();
    if (!(tileSizeCm > 0.0f)) return;
    const f32 centreX = landscapeData_.originCm[0] + tileSizeCm * 0.5f;
    const f32 centreY = landscapeData_.originCm[1] + tileSizeCm * 0.5f;

    const landscape::TileCoord camTile =
        landscape::tileAt(cameraXCm, cameraYCm, centreX, centreY, tileSizeCm);
    if (lastCameraTileValid_ && camTile == lastCameraTile_) return;
    lastCameraTile_ = camTile;
    lastCameraTileValid_ = true;

    // A tile an authored section already covers (over a quarter of its area) is not built: the section draws there.
    const auto coveredByExtra = [&](const landscape::TileCoord& t) {
        const f32 x0 = centreX + static_cast<f32>(t.tx) * tileSizeCm - tileSizeCm * 0.5f;
        const f32 y0 = centreY + static_cast<f32>(t.ty) * tileSizeCm - tileSizeCm * 0.5f;
        for (const Section& e : extras_) {
            const f32 ox = std::min(x0 + tileSizeCm, e.data.boundsMax[0]) - std::max(x0, e.data.boundsMin[0]);
            const f32 oy = std::min(y0 + tileSizeCm, e.data.boundsMax[1]) - std::max(y0, e.data.boundsMin[1]);
            if (ox > 0.0f && oy > 0.0f && ox * oy > 0.25f * tileSizeCm * tileSizeCm) return true;
        }
        return false;
    };

    // Which coordinates should be resident now -- a (2R+1)x(2R+1) window around the camera's own
    // tile, minus (0,0) itself (that is the home tile, not a ring tile).
    std::vector<landscape::TileCoord> want;
    want.reserve(kMaxSectionsResident);
    for (i32 dy = -kRingRadius; dy <= kRingRadius; ++dy)
        for (i32 dx = -kRingRadius; dx <= kRingRadius; ++dx) {
            const landscape::TileCoord t{camTile.tx + dx, camTile.ty + dy};
            if (t.tx == 0 && t.ty == 0) continue;
            if (coveredByExtra(t)) continue;
            want.push_back(t);
        }

    // Evict whatever is resident but no longer wanted.
    for (auto it = landscapeRingTiles_.begin(); it != landscapeRingTiles_.end(); ) {
        const bool stillWanted = std::find(want.begin(), want.end(), it->first) != want.end();
        if (!stillWanted) {
            if (it->second.renderer && device) it->second.renderer->forgetAll(*device);
            it = landscapeRingTiles_.erase(it);
        } else {
            ++it;
        }
    }

    // Synthesize and build whatever is wanted but not yet resident. Same sample count as the
    // authored section -- it already validated against LandscapeTree::build's tiling rule, so a ring
    // tile built the same way is guaranteed to validate too.
    for (const landscape::TileCoord& t : want) {
        if (landscapeRingTiles_.find(t) != landscapeRingTiles_.end()) continue;
        RingTile tile;
        if (!landscape::synthesizeTerrainTile(t, centreX, centreY, tileSizeCm,
                                              landscapeData_.sampleCount, landscapeNoiseParams_,
                                              tile.data, frame_.x, frame_.y)) {
            AVER_WARN("[Landscape] could not synthesize ring tile ({}, {})", t.tx, t.ty);
            continue;
        }
        std::string why;
        if (!tile.tree.build(tile.data, landscapeTree_.nodeQuads(), &why)) {
            AVER_WARN("[Landscape] ring tile ({}, {}) quadtree would not build: {}", t.tx, t.ty, why);
            continue;
        }
        // Every rim of a ring tile borders SOMETHING -- the home tile, or another ring tile -- never
        // open air, so all four get the same generous floor the home tile's outer rim got in
        // loadLandscape().
        tile.tree.widenRimSkirts(2.0f * landscapeNoiseParams_.amplitudeCm);
        tile.tree.resetHysteresis();
        tile.renderer = std::make_unique<landscape::LandscapeRenderer>(kMaxResidentNodesPerTile);
        tile.renderer->setOrigin(frame_.x, frame_.y);
        // A tile born mid-session has to be told the surface too, or the ring renders untextured
        // around a textured home section -- a seam that moves with the camera.
#if AVER_MODULE_PBR
        if (content && materials) applySurface(*tile.renderer, *content, *materials, true);
#endif
        landscapeRingTiles_.emplace(t, std::move(tile));
    }

    AVER_INFO("[Landscape] ring around tile ({}, {}): {} tile(s) resident, {} draws/tile, "
              "{} resident-node cap/tile", camTile.tx, camTile.ty, landscapeRingTiles_.size(),
              kMaxDrawsPerTile, kMaxResidentNodesPerTile);
}

void GameLandscape::draw(rhi::IDevice& device, const Vec3& eye, const Mat4& viewProj, f32 viewportHeightPx,
                          GameContent* content, pbr::MaterialSystem* materials, f32 fovYRad) {
    if (!loaded_ || !landscapeRenderer_) return;
#if AVER_MODULE_PBR
    // Bind the level's terrain material the first frame the material system is ready. Level load
    // cannot be trusted to be late enough: MaterialSystem::ready() also needs its GPU side up, with no
    // guaranteed order against the load, so the test runs every frame and self-heals.
    // wave3 layers: notice an edited terrain material (and rebind), then keep the weight textures current.
    const bool terrainChanged = content && materials && !landscapeMaterial_.empty() && materials->ready() &&
                                syncTerrain(*content, *materials);
    if (content && materials && !landscapeMaterial_.empty() &&
        (terrainChanged || !landscapeRenderer_->hasSurfaceBinding() || !landscapeUvTilingResolved_))
        applySurfaceToAll(&device, *content, *materials);
    flushWeights(&device, materials);
#else
    (void)content; (void)materials;
#endif

    landscape::SelectParams lp;
    lp.cameraCm[0] = eye.x; lp.cameraCm[1] = eye.y; lp.cameraCm[2] = eye.z;
    // The same projection scale trifactor::projScale uses, over this frame's real viewport height:
    // a mismatched scale refines terrain at the wrong distance without failing visibly.
    lp.projScale = viewportHeightPx / (2.0f * std::tan(fovYRad * 0.5f));
    lp.useFrustum = true;
    f32 vpm[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) vpm[r * 4 + c] = viewProj.m[r][c];
    lp.frustum = landscape::Frustum::fromViewProj(vpm);

    // Every section and ring tile is a candidate. Those inside the draw radius and the frustum compete for one draw
    // budget, nearest first, and only the nearest kMaxDrawnSections draw at all.
    ++drawFrame_;
    struct Cand {
        f32 dist2;
        const fmt::OcLandData* data;
        landscape::LandscapeTree* tree;
        landscape::LandscapeRenderer* renderer;
        u64* lastDrawn;
    };
    std::vector<Cand> cands;
    cands.reserve(1 + extras_.size() + landscapeRingTiles_.size());
    // A section that has not drawn for a while gives its GPU meshes back (the cache would otherwise hold its last view).
    const auto idleSweep = [&](landscape::LandscapeRenderer* r, u64& last) {
        if (r && last != 0 && drawFrame_ - last > kIdleFramesBeforeForget) { r->forgetAll(device); last = 0; }
    };
    const auto consider = [&](const fmt::OcLandData& d, landscape::LandscapeTree& t, landscape::LandscapeRenderer* r, u64& last) {
        if (!r) return;
        const f32 dx = std::max({d.boundsMin[0] - eye.x, eye.x - d.boundsMax[0], 0.0f});
        const f32 dy = std::max({d.boundsMin[1] - eye.y, eye.y - d.boundsMax[1], 0.0f});
        const f32 d2 = dx * dx + dy * dy;
        if (d2 <= kDrawRadiusCm * kDrawRadiusCm) {
            const f32 ex = d.boundsMax[0] - d.boundsMin[0], ey = d.boundsMax[1] - d.boundsMin[1], ez = d.boundsMax[2] - d.boundsMin[2];
            const f32 centre[3] = {(d.boundsMin[0] + d.boundsMax[0]) * 0.5f, (d.boundsMin[1] + d.boundsMax[1]) * 0.5f,
                                   (d.boundsMin[2] + d.boundsMax[2]) * 0.5f};
            if (lp.frustum.intersectsSphere(centre, 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez))) {
                cands.push_back({d2, &d, &t, r, &last});
                return;
            }
        }
        idleSweep(r, last);
    };
    consider(landscapeData_, landscapeTree_, landscapeRenderer_.get(), primaryLastDrawn_);
    for (Section& e : extras_) consider(e.data, e.tree, e.renderer.get(), e.lastDrawn);
    for (auto& kv : landscapeRingTiles_) consider(kv.second.data, kv.second.tree, kv.second.renderer.get(), kv.second.lastDrawn);

    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.dist2 < b.dist2; });
    for (usize i = kMaxDrawnSections; i < cands.size(); ++i) idleSweep(cands[i].renderer, *cands[i].lastDrawn);
    if (cands.size() > kMaxDrawnSections) cands.resize(kMaxDrawnSections);

    // Sections carry their world position in every sample (OcLandData::worldAt folds originCm in), so
    // the transform draw() applies on top is identity.
    static const f32 kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    u32 remaining = kTotalDraws;
    for (usize i = 0; i < cands.size(); ++i) {
        // An even share of what is left: a nearer section that needs less leaves more to the farther ones.
        const u32 left = static_cast<u32>(cands.size() - i);
        lp.maxDraws = std::max<u32>(1u, std::min(remaining, std::max(kMinDrawsPerSection, remaining / left)));
        landscape::SelectResult sel;
        cands[i].tree->select(lp, sel);
        cands[i].renderer->draw(device, *cands[i].data, *cands[i].tree, sel, kIdentity, landscapeUvTilingCm_);
        *cands[i].lastDrawn = drawFrame_;
        remaining -= std::min(remaining, static_cast<u32>(sel.nodes.size()));
    }
}

// Puts a section just read from disk into the current frame. originCm is the file's f32, equal to homeAbs_.
void GameLandscape::placeInFrame(fmt::OcLandData& d) const {
    if (frame_.x == 0 && frame_.y == 0) return;
    const f32 nx = static_cast<f32>(homeAbs_[0] - static_cast<f64>(frame_.x));
    const f32 ny = static_cast<f32>(homeAbs_[1] - static_cast<f64>(frame_.y));
    const f32 dx = nx - d.originCm[0], dy = ny - d.originCm[1];
    d.originCm[0] = nx;
    d.originCm[1] = ny;
    d.boundsMin[0] += dx; d.boundsMax[0] += dx;
    d.boundsMin[1] += dy; d.boundsMax[1] += dy;
}

// The floating origin moved to `o` (LARGE_WORLD 2.2.6). Everything resident is translated, nothing is rebuilt: the data and
// the tree follow by the delta, the renderers take the new frame and draw their old meshes translated.
void GameLandscape::moveFrameTo(const rhi::ShiftOrigin& o) {
    if (o.x == frame_.x && o.y == frame_.y) return;
    const rhi::ShiftOrigin was = frame_;
    frame_ = o;
    // What every position moved by: minus the origin's step, a whole number of regions and so exact in f32.
    const f32 moveX = -static_cast<f32>(frame_.x - was.x), moveY = -static_cast<f32>(frame_.y - was.y);
    if (landscapeRenderer_) landscapeRenderer_->setOrigin(frame_.x, frame_.y);
    if (loaded_) {
        // The home section: its origin from the exact absolute one (one rounding), the tree and bounds by what moved.
        landscapeData_.originCm[0] = static_cast<f32>(homeAbs_[0] - static_cast<f64>(frame_.x));
        landscapeData_.originCm[1] = static_cast<f32>(homeAbs_[1] - static_cast<f64>(frame_.y));
        landscapeData_.boundsMin[0] += moveX; landscapeData_.boundsMax[0] += moveX;
        landscapeData_.boundsMin[1] += moveY; landscapeData_.boundsMax[1] += moveY;
        landscapeTree_.translate(moveX, moveY);
        // A painted section's parameter texture carries the section's origin (the weightmap lookup): upload it again.
        if (landscapeData_.hasLayers()) {
            weightsDirty_ = true;
#if AVER_MODULE_PBR
            paramsDirty_ = true;
#endif
        }
    }
    for (Section& e : extras_) {
        e.data.originCm[0] = static_cast<f32>(e.homeAbs[0] - static_cast<f64>(frame_.x));
        e.data.originCm[1] = static_cast<f32>(e.homeAbs[1] - static_cast<f64>(frame_.y));
        e.data.boundsMin[0] += moveX; e.data.boundsMax[0] += moveX;
        e.data.boundsMin[1] += moveY; e.data.boundsMax[1] += moveY;
        e.tree.translate(moveX, moveY);
        if (e.renderer) e.renderer->setOrigin(frame_.x, frame_.y);
#if AVER_MODULE_PBR
        if (e.data.hasLayers()) e.paramsDirty = e.weightsDirty = true;   // its parameter texture carries the origin
#endif
    }
    // Ring tiles were synthesised in the old frame; each follows by the same delta.
    for (auto& kv : landscapeRingTiles_) {
        RingTile& tile = kv.second;
        tile.data.originCm[0] += moveX; tile.data.originCm[1] += moveY;
        tile.data.boundsMin[0] += moveX; tile.data.boundsMax[0] += moveX;
        tile.data.boundsMin[1] += moveY; tile.data.boundsMax[1] += moveY;
        tile.tree.translate(moveX, moveY);
        if (tile.renderer) tile.renderer->setOrigin(frame_.x, frame_.y);
    }
}

void GameLandscape::setOriginCm(f32 x, f32 y, f32 z) {
    homeAbs_[0] = static_cast<f64>(x) + static_cast<f64>(frame_.x);   // x, y are scene-frame; the sidecar is absolute
    homeAbs_[1] = static_cast<f64>(y) + static_cast<f64>(frame_.y);
    landscapeData_.originCm[0] = x;
    landscapeData_.originCm[1] = y;
    landscapeData_.originCm[2] = z;
    dirty_ = true;
}

// Writes one stored rect of samples back into the section and rebuilds what it touched. SHARED BY
// UNDO AND REDO, which differ only in which of the two stored buffers the caller passes as `src`.
// The rebuild afterwards is the same work sculpt() does per tick: the quadtree's per-level error
// and skirt values are maxima over the level, so any height change can move them.
bool GameLandscape::applyHeightRect(u32 x0, u32 y0, u32 x1, u32 y1, const std::vector<f32>& src) {
    if (!loaded_ || src.empty()) return false;
    const u32 n = landscapeData_.sampleCount;
    if (x1 >= n || y1 >= n) return false;   // section was reloaded at a different size
    const u32 w = x1 - x0 + 1;
    for (u32 y = y0; y <= y1; ++y)
        for (u32 x = x0; x <= x1; ++x)
            landscapeData_.heights[y * n + x] = src[(y - y0) * w + (x - x0)];

    // Bounds are derived, not stored, and applyBrush is what normally recomputes them -- writing
    // samples directly bypasses that, so a stroke undone at the section's high point would leave
    // boundsMax describing terrain that no longer exists, and the quadtree's culling with it.
    landscapeData_.boundsMin[2] = landscapeData_.boundsMax[2] = landscapeData_.heights.empty() ? 0.0f
                                                                             : landscapeData_.heights[0];
    for (f32 h : landscapeData_.heights) {
        landscapeData_.boundsMin[2] = std::fmin(landscapeData_.boundsMin[2], h);
        landscapeData_.boundsMax[2] = std::fmax(landscapeData_.boundsMax[2], h);
    }

    std::string why;
    if (!landscapeTree_.build(landscapeData_, landscapeTree_.nodeQuads(), &why)) {
        AVER_ERROR("[Landscape] undo left the section unbuildable: {}", why);
        return false;
    }
    landscapeTree_.widenRimSkirts(2.0f * landscapeNoiseParams_.amplitudeCm);
    landscapeTree_.resetHysteresis();

    // THE GPU HALF IS DEFERRED ONE FRAME, why this takes no device: undo()/redo() are reachable
    // from places with no device to hand. The heightfield and quadtree update HERE, synchronously,
    // so queries see the undone state immediately; only the cached GPU meshes lag by a frame,
    // invisibly (see flushPendingInvalidate).
    pendingInvalidate_ = true;
    pendingX0_ = x0; pendingY0_ = y0;
    pendingX1_ = x1; pendingY1_ = y1;
    dirty_ = true;
    collisionStale_ = true;   // see rebuildCollisionIfStale
    return true;
}

// Drains the invalidation applyHeightRect latches. Called once per frame from the host's own
// render, which has the device forgetOverlapping needs.
void GameLandscape::flushPendingInvalidate(rhi::IDevice* device) {
    if (!pendingInvalidate_) return;
    pendingInvalidate_ = false;
    if (landscapeRenderer_ && device)
        landscapeRenderer_->forgetOverlapping(*device, landscapeTree_,
                                              pendingX0_, pendingY0_, pendingX1_, pendingY1_);
}

// Starts a stroke: remembers that nothing has been touched yet. The BEFORE samples are captured
// lazily as the rect grows (see growStroke) rather than up front, because at stroke start the rect
// is not known -- a drag can wander anywhere.
void GameLandscape::beginStroke() {
    strokeActive_ = true;
    strokeEmpty_ = true;
    strokeBefore_.clear();
}

// Unions this tick's touched rect into the stroke's, capturing the pre-stroke heights of anything
// newly covered.
// THE ORDER MATTERS: this must run BEFORE the tick's own brush write, or the "before" it captures
// is already the "after".
void GameLandscape::growStroke(const landscape::BrushRect& r) {
    if (r.empty || !strokeActive_) return;
    const u32 n = landscapeData_.sampleCount;
    u32 nx0 = r.x0, ny0 = r.y0, nx1 = r.x1, ny1 = r.y1;
    if (!strokeEmpty_) {
        nx0 = std::min(nx0, strokeX0_); ny0 = std::min(ny0, strokeY0_);
        nx1 = std::max(nx1, strokeX1_); ny1 = std::max(ny1, strokeY1_);
    }
    if (nx1 >= n || ny1 >= n) return;

    // The rect grew, so the captured buffer has to be rebuilt at the new size. Samples already
    // inside the old rect keep their ORIGINAL pre-stroke value -- copied across from the old
    // buffer, not re-read from the section, which by now holds painted values.
    const u32 nw = nx1 - nx0 + 1, nh = ny1 - ny0 + 1;
    std::vector<f32> grown(static_cast<size_t>(nw) * nh);
    for (u32 y = ny0; y <= ny1; ++y) {
        for (u32 x = nx0; x <= nx1; ++x) {
            const bool inOld = !strokeEmpty_ && x >= strokeX0_ && x <= strokeX1_ &&
                               y >= strokeY0_ && y <= strokeY1_;
            grown[(y - ny0) * nw + (x - nx0)] =
                inOld ? strokeBefore_[(y - strokeY0_) * (strokeX1_ - strokeX0_ + 1) + (x - strokeX0_)]
                      : landscapeData_.heights[y * n + x];
        }
    }
    strokeBefore_ = std::move(grown);
    strokeX0_ = nx0; strokeY0_ = ny0; strokeX1_ = nx1; strokeY1_ = ny1;
    strokeEmpty_ = false;
}

// Ends a stroke and reports what changed, for the caller to turn into one undo entry.
bool GameLandscape::endStroke(std::vector<f32>& before, std::vector<f32>& after,
                               u32& x0, u32& y0, u32& x1, u32& y1) {
    if (!strokeActive_) return false;
    strokeActive_ = false;
    if (strokeEmpty_ || !loaded_) { strokeBefore_.clear(); return false; }

    const u32 n = landscapeData_.sampleCount;
    const u32 w = strokeX1_ - strokeX0_ + 1, h = strokeY1_ - strokeY0_ + 1;
    after.resize(static_cast<size_t>(w) * h);
    for (u32 y = strokeY0_; y <= strokeY1_; ++y)
        for (u32 x = strokeX0_; x <= strokeX1_; ++x)
            after[(y - strokeY0_) * w + (x - strokeX0_)] = landscapeData_.heights[y * n + x];
    before = std::move(strokeBefore_);
    x0 = strokeX0_; y0 = strokeY0_; x1 = strokeX1_; y1 = strokeY1_;

    // A stroke that changed nothing -- clicking on terrain already at the flatten target, or a
    // Smooth pass over a plane -- reports no entry. Otherwise every stray click would cost the
    // user a Ctrl+Z that appears to do nothing.
    if (before == after) { strokeBefore_.clear(); return false; }
    strokeBefore_.clear();
    return true;
}

// One sculpt tick: grows the active stroke over the brush's footprint (before applying it, so the
// "before" growStroke captures is not already the "after"), applies the brush, and -- if anything
// was touched -- rebuilds whatever it may have moved.
landscape::BrushRect GameLandscape::sculpt(rhi::IDevice* device, const landscape::BrushParams& p, f32 amount) {
    growStroke(landscape::brushRect(landscapeData_, p));

    const landscape::BrushRect touched = landscape::applyBrush(landscapeData_, p, amount);
    if (!touched.empty) {
        // REBUILDS THE WHOLE TREE, not just the touched nodes: LandscapeTree::build() has no
        // incremental form (a coarser node's errorCm/skirtCm/radius are per-level MAXIMA, so a
        // local change can in principle move any of them). Proportional to the section's total
        // sample count, not the brush footprint -- cheap here, but real on a large section. Only
        // the GPU mesh cache invalidation below is footprint-local.
        std::string why;
        if (landscapeTree_.build(landscapeData_, landscapeTree_.nodeQuads(), &why)) {
            landscapeTree_.widenRimSkirts(2.0f * landscapeNoiseParams_.amplitudeCm);
            landscapeTree_.resetHysteresis();
            if (landscapeRenderer_ && device)
                landscapeRenderer_->forgetOverlapping(*device, landscapeTree_,
                                                       touched.x0, touched.y0, touched.x1, touched.y1);
        } else {
            AVER_ERROR("[Landscape] sculpt left the section unbuildable: {}", why);
        }
        dirty_ = true;
        collisionStale_ = true;   // see rebuildCollisionIfStale
    }
    return touched;
}

// Writes the resident section back to path(). A sculpt is fully functional in memory without this;
// it's the one place edits actually reach disk.
bool GameLandscape::save(std::string* why) {
    if (!loaded_ || landscapePath_.empty()) return false;
    // The sidecar is ABSOLUTE: write the exact origin, then put the scene-frame one back. At origin zero both are the same f32.
    const f32 localOx = landscapeData_.originCm[0], localOy = landscapeData_.originCm[1];
    landscapeData_.originCm[0] = static_cast<f32>(homeAbs_[0]);
    landscapeData_.originCm[1] = static_cast<f32>(homeAbs_[1]);
    const bool wrote = fmt::saveOcLand(landscapePath_, landscapeData_, why);
    landscapeData_.originCm[0] = localOx;
    landscapeData_.originCm[1] = localOy;
    if (!wrote) return false;
    dirty_ = false;
    // Saving is the natural commit point for a sculpt, so it is where the collision snapshot
    // catches up with the heights -- see rebuildCollision's own comment on why this is not done
    // per stroke.
    rebuildCollision();
    return true;
}

// ---- wave3 layers ----
namespace {

constexpr usize kLayerCap = 8;

void copyUtf8(char* dst, i32 cap, const std::string& s) {
    if (!dst || cap <= 0) return;
    const usize n = std::min<usize>(s.size(), static_cast<usize>(cap) - 1);
    std::copy(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n), dst);
    dst[n] = 0;
}

} // namespace

void GameLandscape::remapFor(const fmt::OcLandData& d, std::array<i32, 8>& out) const {
    out.fill(-1);
    const std::vector<std::string>& names = d.layerNames;
    for (usize s = 0; s < names.size() && s < kLayerCap; ++s)
        for (usize m = 0; m < layerTable_.size(); ++m)
            if (layerTable_[m].name == names[s]) { out[s] = static_cast<i32>(m); break; }
}

void GameLandscape::rebuildRemap() {
    remapFor(landscapeData_, sectionToMaterial_);
    weightsDirty_ = true;
    for (Section& e : extras_) {
        remapFor(e.data, e.remap);
#if AVER_MODULE_PBR
        e.weightsDirty = true;
#endif
    }
}

void GameLandscape::setLayerTable(std::vector<LayerInfo> layers) {
    if (layers.size() > kLayerCap) layers.resize(kLayerCap);
    layerTable_ = std::move(layers);
    rebuildRemap();
    registerLayerSurfaces();
    updateSurfaceSampler();
}

void GameLandscape::registerLayerSurfaces() {
    // Find-or-add: a surface the project's Surfaces.ocsurf already names keeps ITS numbers (loaded first, so authored wins).
    for (const LayerInfo& l : layerTable_)
        registerSurfaceIfAbsent(l.surfaceName(), l.friction, l.restitution);
}

const std::string& GameLandscape::materialLayerName(u32 i) const {
    static const std::string kNone;
    return i < layerTable_.size() ? layerTable_[i].name : kNone;
}

i32 GameLandscape::surfaceThunk(f32 x, f32 y, f32* outFriction, f32* outRestitution, void* user) {
    const auto* self = static_cast<const GameLandscape*>(user);
    f32 fr = 0.0f, re = 0.0f;
    if (!self || !self->surfaceFrictionAt(x, y, fr, re)) return 0;
    if (outFriction) *outFriction = fr;
    if (outRestitution) *outRestitution = re;
    return 1;
}

void GameLandscape::updateSurfaceSampler() {
#if AVER_MODULE_PHYSICS
    // The heightfield bodies take the layers' friction only while there are layers to ask.
    const i32 on = layerTable_.empty() ? 0 : 1;
    if (on) aver_phys_set_surface_sampler(&GameLandscape::surfaceThunk, this);
    if (landscapeBody_ >= 0) aver_phys_body_set_surface_sampled(landscapeBody_, on);
    for (const Section& e : extras_)
        if (e.body >= 0) aver_phys_body_set_surface_sampled(e.body, on);
#endif
}

bool GameLandscape::ensureLayer(const std::string& name, u32& outIndex) {
    if (!loaded_ || wStrokeActive_ || layerTable_.empty()) return false;
    if (!landscapeData_.hasLayers()) {
        u32 base = 0;
        if (!landscape::ensureLayer(landscapeData_, layerTable_.front().name, base)) return false;
    }
    if (!landscape::ensureLayer(landscapeData_, name, outIndex)) return false;
    dirty_ = true;
    rebuildRemap();
    return true;
}

bool GameLandscape::removeLayer(u32 index) {
    if (!loaded_ || wStrokeActive_ || !landscape::removeLayer(landscapeData_, index)) return false;
    dirty_ = true;
    rebuildRemap();
    return true;
}

void GameLandscape::fillLayer(u32 layer) {
    if (!loaded_ || !landscapeData_.layersValid() || layer >= landscapeData_.layerNames.size()) return;
    if (wStrokeActive_) growWeightStroke(landscape::BrushRect{0, 0, landscapeData_.sampleCount - 1, landscapeData_.sampleCount - 1, false});
    landscape::fillLayer(landscapeData_, layer);
    weightsDirty_ = dirty_ = true;
}

void GameLandscape::autoFillBySlope(u32 layer, f32 minSlopeDeg, f32 maxSlopeDeg, f32 minZCm, f32 maxZCm, f32 featherDeg, f32 amount) {
    if (!loaded_ || !landscapeData_.layersValid() || layer >= landscapeData_.layerNames.size()) return;
    if (wStrokeActive_) growWeightStroke(landscape::BrushRect{0, 0, landscapeData_.sampleCount - 1, landscapeData_.sampleCount - 1, false});
    landscape::autoFillBySlope(landscapeData_, layer, minSlopeDeg, maxSlopeDeg, minZCm, maxZCm, featherDeg, amount);
    weightsDirty_ = dirty_ = true;
}

landscape::BrushRect GameLandscape::paint(rhi::IDevice*, const landscape::PaintParams& p, f32 amount) {
    if (!loaded_ || !landscapeData_.layersValid()) return landscape::BrushRect{};
    growWeightStroke(landscape::paintRect(landscapeData_, p));
    const landscape::BrushRect touched = landscape::applyPaint(landscapeData_, p, amount);
    if (!touched.empty) weightsDirty_ = dirty_ = true;
    return touched;
}

void GameLandscape::beginWeightStroke() {
    wStrokeActive_ = true;
    wStrokeEmpty_ = true;
    wStrokeBefore_.clear();
    wStrokePlanes_ = landscapeData_.layerNames.empty() ? 0u : static_cast<u32>(landscapeData_.layerNames.size() - 1);
}

void GameLandscape::growWeightStroke(const landscape::BrushRect& r) {
    if (r.empty || !wStrokeActive_ || wStrokePlanes_ == 0) return;
    const u32 n = landscapeData_.sampleCount;
    const usize nn = static_cast<usize>(n) * n;
    if (landscapeData_.weights.size() < static_cast<usize>(wStrokePlanes_) * nn) return;
    u32 nx0 = r.x0, ny0 = r.y0, nx1 = r.x1, ny1 = r.y1;
    if (!wStrokeEmpty_) {
        nx0 = std::min(nx0, wStrokeX0_); ny0 = std::min(ny0, wStrokeY0_);
        nx1 = std::max(nx1, wStrokeX1_); ny1 = std::max(ny1, wStrokeY1_);
    }
    if (nx1 >= n || ny1 >= n) return;
    // Re-capture at the grown size: samples already inside the old rect keep their ORIGINAL pre-stroke value.
    const u32 nw = nx1 - nx0 + 1, nh = ny1 - ny0 + 1;
    const usize oldW = wStrokeEmpty_ ? 0 : wStrokeX1_ - wStrokeX0_ + 1, oldH = wStrokeEmpty_ ? 0 : wStrokeY1_ - wStrokeY0_ + 1;
    std::vector<u8> grown(static_cast<usize>(wStrokePlanes_) * nw * nh);
    for (u32 pl = 0; pl < wStrokePlanes_; ++pl)
        for (u32 y = ny0; y <= ny1; ++y)
            for (u32 x = nx0; x <= nx1; ++x) {
                const bool inOld = !wStrokeEmpty_ && x >= wStrokeX0_ && x <= wStrokeX1_ && y >= wStrokeY0_ && y <= wStrokeY1_;
                grown[(static_cast<usize>(pl) * nh + (y - ny0)) * nw + (x - nx0)] =
                    inOld ? wStrokeBefore_[(static_cast<usize>(pl) * oldH + (y - wStrokeY0_)) * oldW + (x - wStrokeX0_)]
                          : landscapeData_.weights[static_cast<usize>(pl) * nn + static_cast<usize>(y) * n + x];
            }
    wStrokeBefore_ = std::move(grown);
    wStrokeX0_ = nx0; wStrokeY0_ = ny0; wStrokeX1_ = nx1; wStrokeY1_ = ny1;
    wStrokeEmpty_ = false;
}

bool GameLandscape::endWeightStroke(std::vector<u8>& before, std::vector<u8>& after, u32& x0, u32& y0, u32& x1, u32& y1, u32& planes) {
    if (!wStrokeActive_) return false;
    wStrokeActive_ = false;
    if (wStrokeEmpty_ || !loaded_ || wStrokePlanes_ == 0) { wStrokeBefore_.clear(); return false; }
    const u32 n = landscapeData_.sampleCount;
    const usize nn = static_cast<usize>(n) * n;
    const u32 w = wStrokeX1_ - wStrokeX0_ + 1, h = wStrokeY1_ - wStrokeY0_ + 1;
    after.resize(static_cast<usize>(wStrokePlanes_) * w * h);
    for (u32 pl = 0; pl < wStrokePlanes_; ++pl)
        for (u32 y = wStrokeY0_; y <= wStrokeY1_; ++y)
            for (u32 x = wStrokeX0_; x <= wStrokeX1_; ++x)
                after[(static_cast<usize>(pl) * h + (y - wStrokeY0_)) * w + (x - wStrokeX0_)] =
                    landscapeData_.weights[static_cast<usize>(pl) * nn + static_cast<usize>(y) * n + x];
    before = std::move(wStrokeBefore_);
    x0 = wStrokeX0_; y0 = wStrokeY0_; x1 = wStrokeX1_; y1 = wStrokeY1_;
    planes = wStrokePlanes_;
    wStrokeBefore_.clear();
    if (before == after) return false;   // a stroke that changed nothing makes no undo entry
    weightsDirty_ = true;                // upload at once, not on the 100 ms timer
    return true;
}

bool GameLandscape::applyWeightRect(u32 x0, u32 y0, u32 x1, u32 y1, u32 planes, const std::vector<u8>& src) {
    if (!loaded_ || src.empty() || !landscapeData_.layersValid()) return false;
    const u32 n = landscapeData_.sampleCount;
    if (x1 >= n || y1 >= n || x0 > x1 || y0 > y1) return false;
    const u32 stored = landscapeData_.layerNames.empty() ? 0u : static_cast<u32>(landscapeData_.layerNames.size() - 1);
    if (planes == 0 || planes > stored) return false;
    const u32 w = x1 - x0 + 1, h = y1 - y0 + 1;
    if (src.size() != static_cast<usize>(planes) * w * h) return false;
    const usize nn = static_cast<usize>(n) * n;
    for (u32 pl = 0; pl < planes; ++pl)
        for (u32 y = y0; y <= y1; ++y)
            for (u32 x = x0; x <= x1; ++x)
                landscapeData_.weights[static_cast<usize>(pl) * nn + static_cast<usize>(y) * n + x] =
                    src[(static_cast<usize>(pl) * h + (y - y0)) * w + (x - x0)];
    weightsDirty_ = dirty_ = true;
    return true;
}

bool GameLandscape::surfaceFrictionAt(f32 xCm, f32 yCm, f32& outFriction, f32& outRestitution) const {
    if (!loaded_ || layerTable_.empty()) return false;
    Hit hit;
    if (!findSection(xCm, yCm, hit)) return false;
    const fmt::OcLandData& d = *hit.data;
    f32 sw[8];
    if (!landscape::layerWeightsAt(d, xCm, yCm, sw)) return false;
    f32 mw[8] = {};
    const usize L = std::max<usize>(d.layerNames.size(), 1);
    for (usize s = 0; s < L && s < kLayerCap; ++s) {
        const i32 c = s < d.layerNames.size() ? (*hit.remap)[s] : 0;
        mw[c >= 0 ? static_cast<usize>(c) : 0] += sw[s];
    }
    f32 fr = 0.0f, re = 0.0f, total = 0.0f;
    for (usize m = 0; m < layerTable_.size(); ++m) {
        fr += layerTable_[m].friction * mw[m];
        re += layerTable_[m].restitution * mw[m];
        total += mw[m];
    }
    if (total <= 1e-6f) return false;
    outFriction = fr / total;
    outRestitution = re / total;
    return true;
}

bool GameLandscape::sampleGround(f32 xCm, f32 yCm, GroundInfo& out) const {
    Hit hit;
    if (!findSection(xCm, yCm, hit)) return false;
    const fmt::OcLandData& d = *hit.data;
    const f32 z = hit.z;
    out = GroundInfo{};
    out.z = z;

    // The slope: central differences one sample apart (across a section seam too), falling back to the centre height at
    // the outer rim.
    const f32 s = d.spacingCm;
    const auto h = [&](f32 x, f32 y) { Hit q; return findSection(x, y, q) ? q.z : z; };
    const f32 dzdx = (h(xCm + s, yCm) - h(xCm - s, yCm)) / (2.0f * s);
    const f32 dzdy = (h(xCm, yCm + s) - h(xCm, yCm - s)) / (2.0f * s);
    const f32 il = 1.0f / std::sqrt(dzdx * dzdx + dzdy * dzdy + 1.0f);
    out.normal[0] = -dzdx * il; out.normal[1] = -dzdy * il; out.normal[2] = il;

    if (layerTable_.empty()) return true;
    f32 sw[8];
    if (!landscape::layerWeightsAt(d, xCm, yCm, sw)) return true;
    const usize L = std::max<usize>(d.layerNames.size(), 1);
    for (usize i = 0; i < L && i < kLayerCap; ++i) {
        const i32 c = i < d.layerNames.size() ? (*hit.remap)[i] : 0;
        out.weights[c >= 0 ? static_cast<usize>(c) : 0] += sw[i];
    }
    out.dominant = landscape::dominantLayer(out.weights, static_cast<u32>(layerTable_.size()));
    f32 fr = 0.0f, re = 0.0f, total = 0.0f;
    for (usize m = 0; m < layerTable_.size(); ++m) {
        fr += layerTable_[m].friction * out.weights[m];
        re += layerTable_[m].restitution * out.weights[m];
        total += out.weights[m];
    }
    if (total > 1e-6f) { out.friction = fr / total; out.restitution = re / total; }
    out.surface = layerTable_[out.dominant].surfaceName();
    return true;
}

void GameLandscape::heightsEdited(rhi::IDevice* device, const landscape::BrushRect& touched) {
    if (touched.empty) return;
    std::string why;
    if (landscapeTree_.build(landscapeData_, landscapeTree_.nodeQuads(), &why)) {
        landscapeTree_.widenRimSkirts(2.0f * landscapeNoiseParams_.amplitudeCm);
        landscapeTree_.resetHysteresis();
        if (landscapeRenderer_ && device)
            landscapeRenderer_->forgetOverlapping(*device, landscapeTree_, touched.x0, touched.y0, touched.x1, touched.y1);
    } else {
        AVER_ERROR("[Landscape] a height edit left the section unbuildable: {}", why);
    }
    dirty_ = true;
    collisionStale_ = true;
}

landscape::BrushRect GameLandscape::carveRiver(rhi::IDevice* device, const landscape::CarvePoint* pts, u32 n,
                                               const landscape::CarveParams& p) {
    if (!loaded_) return landscape::BrushRect{};
    growStroke(landscape::carveRect(landscapeData_, pts, n, p));
    const landscape::BrushRect touched = landscape::carveRiver(landscapeData_, pts, n, p);
    heightsEdited(device, touched);
    return touched;
}

landscape::BrushRect GameLandscape::carveBasin(rhi::IDevice* device, const f32* polyXY, u32 count, f32 levelCm,
                                               const landscape::CarveParams& p) {
    if (!loaded_) return landscape::BrushRect{};
    growStroke(landscape::basinRect(landscapeData_, polyXY, count, p));
    const landscape::BrushRect touched = landscape::carveBasin(landscapeData_, polyXY, count, levelCm, p);
    heightsEdited(device, touched);
    return touched;
}

void GameLandscape::setSurfaceMaterial(const std::string& name) {
    landscapeMaterial_ = name;
    landscapeUvTilingResolved_ = false;   // draw() re-applies the surface (and re-syncs the terrain material) next frame
#if AVER_MODULE_PBR
    baseSig_ = 0;                          // force syncTerrain to look again
#endif
}

void GameLandscape::installProviders() {
#if AVER_MODULE_FRAMEWORK
    aver_fw_set_terrain_provider(
        [](f32 x, f32 y, f32* outZ, f32* outNormal3, i32* outDominant, f32* outWeights, void* user) -> i32 {
            GroundInfo g;
            if (!static_cast<const GameLandscape*>(user)->sampleGround(x, y, g)) return 0;
            if (outZ) *outZ = g.z;
            if (outNormal3) for (int i = 0; i < 3; ++i) outNormal3[i] = g.normal[i];
            if (outDominant) *outDominant = static_cast<i32>(g.dominant);
            if (outWeights) for (int i = 0; i < AVER_FW_TERRAIN_MAX_LAYERS; ++i) outWeights[i] = g.weights[i];
            return 1;
        },
        [](void* user) -> i32 { return static_cast<i32>(static_cast<const GameLandscape*>(user)->materialLayerCount()); },
        [](i32 layer, char* nameBuf, i32 nameCap, char* surfaceBuf, i32 surfaceCap, f32* outFriction, f32* outRestitution,
           void* user) -> i32 {
            const LayerInfo* l = layer < 0 ? nullptr : static_cast<const GameLandscape*>(user)->materialLayer(static_cast<u32>(layer));
            if (!l) return 0;
            copyUtf8(nameBuf, nameCap, l->name);
            copyUtf8(surfaceBuf, surfaceCap, l->surfaceName());
            if (outFriction) *outFriction = l->friction;
            if (outRestitution) *outRestitution = l->restitution;
            return 1;
        },
        this);
#endif
}

#if AVER_MODULE_PBR
namespace {

// Everything that decides how the derived section / ring materials and the layer table must look.
u64 terrainSignature(const pbr::MaterialDesc& d) {
    u64 h = 1469598103934665603ull;   // FNV-1a
    const auto mix = [&h](const void* p, usize n) {
        const u8* b = static_cast<const u8*>(p);
        for (usize i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    const auto str = [&](const std::string& s) { mix(s.data(), s.size()); const u8 z = 0; mix(&z, 1); };
    const auto ref = [&](const pbr::TextureRef& r) { str(r.path); mix(&r.id, sizeof r.id); };
    mix(d.baseColorFactor, sizeof d.baseColorFactor);
    const f32 scalars[] = {d.metallicFactor, d.roughnessFactor, d.normalScale, d.occlusionStrength, d.reflectance, d.f90,
                           d.uvTiling, d.terrain.blendDepth};
    mix(scalars, sizeof scalars);
    mix(&d.terrain.tileTexels, sizeof d.terrain.tileTexels);
    mix(&d.terrain.padTexels, sizeof d.terrain.padTexels);
    mix(&d.graphId, sizeof d.graphId);
    for (const pbr::TerrainLayerDesc& l : d.terrain.layers) {
        str(l.name); str(l.surface); str(l.foliage);
        ref(l.baseColor); ref(l.normal); ref(l.metalRough); ref(l.height);
        const f32 f[] = {l.tilingCm, l.heightBias, l.friction, l.restitution, l.foliageDensity, l.foliageMinWeight};
        mix(f, sizeof f);
    }
    return h;
}

rhi::TextureHandle makeRgba8(rhi::IResourceFactory& res, u32 n, const std::vector<std::vector<u8>>& levels, const char* name) {
    std::vector<const void*> ptrs;
    ptrs.reserve(levels.size());
    for (const std::vector<u8>& l : levels) ptrs.push_back(l.data());
    rhi::TextureDesc d;
    d.width = d.height = n;
    d.mips = static_cast<u32>(levels.size());
    d.format = rhi::Format::RGBA8Unorm;
    d.bind = rhi::ResourceBind::ShaderResource;
    d.initialState = rhi::ResourceState::ShaderResource;
    d.initialData = ptrs.data();
    d.initialDataCount = static_cast<u32>(ptrs.size());
    d.initialRowPitch = n * 4;
    d.debugName = name;
    return res.createTexture(d);
}

} // namespace

void GameLandscape::makeTerrainSectionDescs(const pbr::MaterialDesc& base, pbr::MaterialDesc& sec, pbr::MaterialDesc& ring,
                                            const std::string& rtSuffix) {
    sec = base;
    ring = base;
    sec.name = base.name + "#sec" + rtSuffix;
    ring.name = base.name + "#ring";
    // Slots 3..5 hold the section's weightmaps and parameters under the terrain flag; the ring has none.
    for (u32 i = static_cast<u32>(pbr::TextureSlot::Occlusion); i < pbr::kTextureSlotCount; ++i) {
        sec.textures[i] = pbr::TextureRef{};
        ring.textures[i] = pbr::TextureRef{};
    }
    sec.textures[static_cast<u32>(pbr::TextureSlot::Occlusion)].path = "@rt:land.w0" + rtSuffix;
    sec.textures[static_cast<u32>(pbr::TextureSlot::Emissive)].path = "@rt:land.w1" + rtSuffix;
    sec.textures[static_cast<u32>(pbr::TextureSlot::Layer1BaseColor)].path = "@rt:land.params" + rtSuffix;
}

bool GameLandscape::syncTerrain(GameContent& content, pbr::MaterialSystem& materials) {
    if (landscapeMaterial_.empty() || !materials.ready()) return false;
    lastMaterials_ = &materials;
    pbr::MaterialLibrary& lib = pbr::MaterialLibrary::get();
    const pbr::MaterialHandle h = content.materialForSurface(landscapeMaterial_);
    const pbr::MaterialDesc* d = h && lib.valid(h) ? lib.desc(h) : nullptr;
    if (!d || !d->terrain.enabled()) {
        if (!terrainActive_) return false;
        // The material stopped being layered (edited, or swapped): back to the plain single-material path.
        releaseTerrain(nullptr);
        layerTable_.clear();
        rebuildRemap();
        updateSurfaceSampler();
        return true;
    }
    const u64 sig = terrainSignature(*d);
    if (terrainActive_ && baseMat_ == h && sig == baseSig_) {
        // Unchanged, unless a painted extra still lacks its derived material.
        bool missing = false;
        for (const Section& e : extras_) missing |= e.data.hasLayers() && !e.mat;
        if (!missing) return false;
        syncExtraMaterials(*d);
        return true;
    }

    pbr::MaterialDesc sec, ring;
    makeTerrainSectionDescs(*d, sec, ring);
    if (secMat_) lib.update(secMat_, sec); else secMat_ = lib.create(sec);
    if (ringMat_) lib.update(ringMat_, ring); else ringMat_ = lib.create(ring);
    syncExtraMaterials(*d);

    std::vector<LayerInfo> layers;
    for (const pbr::TerrainLayerDesc& l : d->terrain.layers) {
        LayerInfo li;
        li.name = l.name; li.surface = l.surface; li.foliage = l.foliage;
        li.friction = l.friction; li.restitution = l.restitution;
        li.tilingCm = l.tilingCm; li.foliageDensity = l.foliageDensity; li.foliageMinWeight = l.foliageMinWeight;
        layers.push_back(std::move(li));
    }
    baseMat_ = h;
    baseSig_ = sig;
    terrainActive_ = true;
    paramsDirty_ = true;
    setLayerTable(std::move(layers));   // remap + physics sampler; marks the weights dirty
    AVER_INFO("[Landscape] layered terrain material '{}': {} layer(s)", d->name, d->terrain.layers.size());
    return true;
}

void GameLandscape::syncExtraMaterials(const pbr::MaterialDesc& base) {
    pbr::MaterialLibrary& lib = pbr::MaterialLibrary::get();
    for (usize i = 0; i < extras_.size(); ++i) {
        Section& e = extras_[i];
        if (!e.data.hasLayers()) continue;
        pbr::MaterialDesc sec, ring;
        makeTerrainSectionDescs(base, sec, ring, "." + std::to_string(i + 1));
        if (e.mat) lib.update(e.mat, sec); else e.mat = lib.create(sec);
        e.paramsDirty = e.weightsDirty = true;
    }
}

void GameLandscape::releaseTerrain(rhi::IDevice* device) {
    pbr::MaterialLibrary& lib = pbr::MaterialLibrary::get();
    if (lastMaterials_ && lastMaterials_->ready()) {
        for (const char* nm : {"land.w0", "land.w1", "land.params"}) {
            const rhi::TextureHandle t = lastMaterials_->clearRuntimeTexture(nm);
            if (t) retired_.push_back({t, flushFrame_});
        }
    } else {
        for (const u32 t : {texA_, texB_, texParams_}) if (t) retired_.push_back({t, flushFrame_});
    }
    texA_ = texB_ = texParams_ = 0;
    paramsDirty_ = true;
    for (usize i = 0; i < extras_.size(); ++i) {
        Section& e = extras_[i];
        const std::string suffix = "." + std::to_string(i + 1);
        if (lastMaterials_ && lastMaterials_->ready()) {
            for (const char* nm : {"land.w0", "land.w1", "land.params"}) {
                const rhi::TextureHandle t = lastMaterials_->clearRuntimeTexture(nm + suffix);
                if (t) retired_.push_back({t, flushFrame_});
            }
        } else {
            for (const u32 t : {e.texA, e.texB, e.texParams}) if (t) retired_.push_back({t, flushFrame_});
        }
        e.texA = e.texB = e.texParams = 0;
        e.paramsDirty = e.weightsDirty = true;
        if (e.mat) lib.destroy(e.mat);
        e.mat = 0;
    }
    if (secMat_) lib.destroy(secMat_);
    if (ringMat_) lib.destroy(ringMat_);
    secMat_ = ringMat_ = baseMat_ = 0;
    baseSig_ = 0;
    terrainActive_ = false;
    if (device) {
        if (rhi::IResourceFactory* res = device->resources())
            for (const Retired& r : retired_) res->destroyTexture(r.texture);
        retired_.clear();
        lastMaterials_ = nullptr;
    }
}

void GameLandscape::uploadSectionWeights(rhi::IResourceFactory& res, pbr::MaterialSystem& materials, const pbr::MaterialDesc& base,
                                         const pbr::TerrainTextures& info, const fmt::OcLandData& data,
                                         const std::array<i32, 8>& remap, const std::string& rtSuffix, u32& texA, u32& texB,
                                         u32& texParams, bool& paramsDirty) {
    rhi::TextureHandle a = 0, b = 0, p = 0;
    const u32 n = data.sampleCount;
    const bool painted = data.hasLayers() && data.layerNames.size() >= 2 && data.layersValid();
    bool keepParams = false;
    if (painted) {
        std::vector<u8> imgA, imgB;
        i32 chan[8];
        for (usize i = 0; i < 8; ++i) chan[i] = remap[i];
        landscape::packWeightTextures(data, chan, static_cast<u32>(layerTable_.size()), imgA, imgB);
        std::vector<std::vector<u8>> la, lb;
        landscape::buildMipChainRGBA8(n, imgA, la);
        a = makeRgba8(res, n, la, "terrain weights A");
        // The second map (layers 5..7) only exists when some layer maps there; otherwise the slot keeps the shared zero texture.
        bool needB = false;
        for (usize s = 0; s < data.layerNames.size() && s < 8; ++s)
            needB |= chan[s] >= 5 && static_cast<usize>(chan[s]) < layerTable_.size();
        if (needB) {
            landscape::buildMipChainRGBA8(n, imgB, lb);
            b = makeRgba8(res, n, lb, "terrain weights B");
        }

        // The parameters depend on the layers and the section, not on the weights: reused across strokes.
        keepParams = texParams != 0 && !paramsDirty;
        if (keepParams) p = texParams;
        if (!keepParams) {
            paramsDirty = false;
            f32 texels[pbr::kTerrainParamFloats];
            pbr::TerrainSection sec;
            sec.originXCm = data.originCm[0];
            sec.originYCm = data.originCm[1];
            sec.spacingCm = data.spacingCm;
            sec.n = n;
            pbr::terrainParamTexels(base, info, &sec, base.uvTiling > 0.0f ? 1.0f / base.uvTiling : 0.0f, texels);
            rhi::TextureDesc td;
            td.width = pbr::kTerrainParamWidth;
            td.height = pbr::kTerrainParamHeight;
            td.format = rhi::Format::R32Float;
            td.bind = rhi::ResourceBind::ShaderResource;
            td.initialState = rhi::ResourceState::ShaderResource;
            const void* lv[1] = {texels};
            td.initialData = lv;
            td.initialDataCount = 1;
            td.initialRowPitch = pbr::kTerrainParamWidth * 4;
            td.debugName = "terrain params (section)";
            p = res.createTexture(td);
        }
    }
    // a/b/p stay 0 for an unpainted section: the slots fall back to zero weights and base-only parameters.
    const rhi::TextureHandle ra = materials.setRuntimeTexture("land.w0" + rtSuffix, a);
    const rhi::TextureHandle rb = materials.setRuntimeTexture("land.w1" + rtSuffix, b);
    rhi::TextureHandle rp = 0;
    if (!keepParams) rp = materials.setRuntimeTexture("land.params" + rtSuffix, p);
    for (const rhi::TextureHandle t : {ra, rb, rp}) if (t) retired_.push_back({t, flushFrame_});
    texA = a; texB = b; texParams = p;
}

void GameLandscape::flushWeights(rhi::IDevice* device, pbr::MaterialSystem* materials) {
    ++flushFrame_;
    rhi::IResourceFactory* res = device ? device->resources() : nullptr;
    // Replaced textures wait a few frames: the swap reaches the binding sets on the next MaterialSystem::update().
    if (res) {
        for (auto it = retired_.begin(); it != retired_.end();) {
            if (flushFrame_ - it->frame >= 4) { res->destroyTexture(it->texture); it = retired_.erase(it); }
            else ++it;
        }
    }
    if (!loaded_ || !terrainActive_ || !res || !materials || !materials->ready()) return;
    bool extraDue = false;
    for (const Section& e : extras_) extraDue |= e.weightsDirty && e.mat && e.data.hasLayers();
    if (!weightsDirty_ && !extraDue) return;
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    // Throttled while painting. Every new texture takes a slot in the ray path's bindless table, which only ever grows, so
    // the cadence is gentler than the design's 100 ms and a stroke's end uploads at once.
    const bool primaryDue = weightsDirty_ && !(wStrokeActive_ && now - lastUpload_ < std::chrono::milliseconds(250));
    if (!primaryDue && !extraDue) return;

    pbr::MaterialLibrary& lib = pbr::MaterialLibrary::get();
    const pbr::MaterialDesc* base = lib.desc(baseMat_);
    if (!base || !secMat_) return;
    materials->bindingSet(secMat_);   // building the set resolves the atlas, whose layout the parameters need
    pbr::TerrainTextures info;
    if (!materials->terrainTextures(*base, info)) return;   // no atlas (yet): stay dirty, try again
    lastMaterials_ = materials;

    if (primaryDue) {
        lastUpload_ = now;
        weightsDirty_ = false;
        uploadSectionWeights(*res, *materials, *base, info, landscapeData_, sectionToMaterial_, std::string(), texA_, texB_,
                             texParams_, paramsDirty_);
        lib.touch(secMat_);
    }
    for (usize i = 0; i < extras_.size(); ++i) {
        Section& e = extras_[i];
        if (!e.weightsDirty || !e.mat || !e.data.hasLayers()) continue;
        e.weightsDirty = false;
        uploadSectionWeights(*res, *materials, *base, info, e.data, e.remap, "." + std::to_string(i + 1), e.texA, e.texB,
                             e.texParams, e.paramsDirty);
        lib.touch(e.mat);
    }
}
#else
void GameLandscape::flushWeights(rhi::IDevice*, pbr::MaterialSystem*) {}   // no PBR module: nothing to upload
#endif // AVER_MODULE_PBR
// ---- end wave3 layers ----

} // namespace aver::game

#endif // AVER_MODULE_LANDSCAPE
