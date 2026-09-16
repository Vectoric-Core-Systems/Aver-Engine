#include "aver/game/GameLandscape.hpp"

#if AVER_MODULE_LANDSCAPE

#include "aver/game/GameContent.hpp"
#include "aver/core/Log.hpp"
#include "aver/landscape/HeightfieldRay.hpp"
#include "aver/landscape/PhysicsBridge.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <vector>

#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
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
    landscape::LandscapeTree tree;
    if (!tree.build(data, landscape::kDefaultNodeQuads, &why)) {
        AVER_WARN("[Landscape] '{}' loaded but its quadtree would not build: {}", path, why);
        return false;
    }
    landscapeData_ = std::move(data);
    landscapeTree_ = std::move(tree);
    // The section is tile (0,0) of the ring now -- its outer rim borders a procedural neighbour like
    // any ring tile's does, needing the same generous floor. 2x the noise amplitude is the
    // mathematical bound on how much a ridged-fBm field can vary at all (TerrainNoise.hpp).
    landscapeTree_.widenRimSkirts(2.0f * landscapeNoiseParams_.amplitudeCm);
    landscapeTree_.resetHysteresis();
    landscapeRenderer_ = std::make_unique<landscape::LandscapeRenderer>(kMaxResidentNodesPerTile);
    loaded_ = true;
    landscapePath_ = path;
    dirty_ = false;
    AVER_INFO("[Landscape] '{}' loaded: {} node(s) across {} level(s), {}x{} samples",
              path, landscapeTree_.nodes().size(), landscapeTree_.levelCount(),
              landscapeData_.sampleCount, landscapeData_.sampleCount);
    return true;
}

void GameLandscape::rebuildCollision() {
#if AVER_MODULE_PHYSICS
    if (landscapeBody_ >= 0) { aver_phys_remove_body(landscapeBody_); landscapeBody_ = -1; }
    if (!loaded_ || !aver_phys_ready()) return;
    landscape::PhysicsHeightfield hf;
    if (!landscape::toPhysicsHeightfield(landscapeData_, hf)) {
        AVER_WARN("[Landscape] section is not internally consistent; no collision built");
        return;
    }
    landscapeBody_ = aver_phys_add_heightfield(hf.samples.data(), static_cast<i32>(hf.sampleCount),
                                               hf.spacingCm, hf.cornerCm[0], hf.cornerCm[1],
                                               hf.cornerCm[2]);
    // Deliberately NOT stamped with aver_phys_set_entity: terrain has no owning scene entity in this
    // engine at all. A ray landing on it is a genuine "hit true, entity 0", not a bug.
    if (landscapeBody_ >= 0)
        AVER_INFO("[Landscape] collision body #{} built ({}x{} samples)", landscapeBody_,
                  hf.sampleCount, hf.sampleCount);
    else
        AVER_WARN("[Landscape] physics refused the heightfield; terrain has no collision");
#endif
}

void GameLandscape::unload(rhi::IDevice* device) {
    if (landscapeRenderer_ && device) landscapeRenderer_->forgetAll(*device);
    landscapeRenderer_.reset();
#if AVER_MODULE_PHYSICS
    if (landscapeBody_ >= 0) { aver_phys_remove_body(landscapeBody_); landscapeBody_ = -1; }
#endif
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
}

bool GameLandscape::groundHeightAt(f64 worldXCm, f64 worldYCm, f64& outWorldZCm) const {
    if (!loaded_) return false;
    f32 z = 0.0f;
    if (!landscape::surfaceHeightAt(landscapeData_, static_cast<f32>(worldXCm),
                                    static_cast<f32>(worldYCm), z))
        return false;
    outWorldZCm = static_cast<f64>(z);
    return true;
}

bool GameLandscape::scatterHeightAt(f32 worldXCm, f32 worldYCm, f32& outWorldZCm) const {
    if (landscape::surfaceHeightAt(landscapeData_, worldXCm, worldYCm, outWorldZCm)) return true;
    outWorldZCm = landscape::terrainHeightAt(worldXCm, worldYCm, landscapeNoiseParams_);
    return true;
}

#if AVER_MODULE_PBR
void GameLandscape::applySurface(landscape::LandscapeRenderer& r, GameContent& content,
                                  pbr::MaterialSystem& materials) {
    if (landscapeMaterial_.empty()) return;
    if (!materials.ready()) return;
    const pbr::MaterialHandle h = content.materialForSurface(landscapeMaterial_);
    if (!h) return;
    const pbr::MaterialConstants& mc = materials.constants(h);
    r.setSurfaceBinding(materials.bindingSet(h), &mc, sizeof(pbr::MaterialConstants));

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
    if (landscapeRenderer_) applySurface(*landscapeRenderer_, content, materials);
    for (auto& kv : landscapeRingTiles_)
        if (kv.second.renderer) applySurface(*kv.second.renderer, content, materials);

    // THE CACHED MESHES CARRY THE OLD SCALE, so eviction belongs here (compared once around the whole
    // sweep), not inside applySurface, which runs once per renderer and would leave every OTHER ring
    // tile holding UVs built at the previous tiling.
    if (device && landscapeUvTilingCm_ != wasTiling) {
        if (landscapeRenderer_) landscapeRenderer_->forgetAll(*device);
        for (auto& kv : landscapeRingTiles_)
            if (kv.second.renderer) kv.second.renderer->forgetAll(*device);
        AVER_INFO("[Landscape] texture tiling {:.0f}cm per tile, from material '{}' (was {:.0f}); "
                  "resident nodes dropped to rebuild",
                  landscapeUvTilingCm_, landscapeMaterial_, wasTiling);
    }
}
#endif // AVER_MODULE_PBR

void GameLandscape::loadForLevel(rhi::IDevice* device, const std::string& contentDir,
                                  const std::string& levelPath, const fmt::OcWorldData& w,
                                  GameContent* content, pbr::MaterialSystem* materials) {
    std::string path = pathOverride_;
    bool haveAt = false;
    f64 at[3] = {0, 0, 0};

    // The level's own LANDSCAPE record, if it has one -- resolved against `contentDir` and placed at
    // the record's own `at`. Skipped entirely when setPathOverride() supplied a path already.
    if (path.empty() && !w.landscapes.empty()) {
        const fmt::OcLandscapePlacement& lp = w.landscapes.front();
        if (w.landscapes.size() > 1)
            AVER_WARN("[Landscape] level declares {} LANDSCAPE sections; the runtime holds one and "
                      "is using '{}'. Tiling several sections is not implemented.",
                      w.landscapes.size(), lp.name.empty() ? lp.section : lp.name);
        // Taken even when the section path below fails: the material is a property of the level's
        // terrain, not of which file the heights came from, and the .ocland fallback convention still
        // wants it.
        landscapeMaterial_ = lp.material;
        if (!lp.section.empty()) {
            path = contentDir.empty() ? lp.section : contentDir + "\\" + lp.section;
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
        landscapeData_.originCm[0] = static_cast<f32>(at[0]);
        landscapeData_.originCm[1] = static_cast<f32>(at[1]);
        landscapeData_.originCm[2] = static_cast<f32>(at[2]);
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
    rebuildCollision();
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

    // Which coordinates should be resident now -- a (2R+1)x(2R+1) window around the camera's own
    // tile, minus (0,0) itself (that is the home tile, not a ring tile).
    std::vector<landscape::TileCoord> want;
    want.reserve(kMaxSectionsResident);
    for (i32 dy = -kRingRadius; dy <= kRingRadius; ++dy)
        for (i32 dx = -kRingRadius; dx <= kRingRadius; ++dx) {
            const landscape::TileCoord t{camTile.tx + dx, camTile.ty + dy};
            if (t.tx == 0 && t.ty == 0) continue;
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
                                              tile.data)) {
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
        // A tile born mid-session has to be told the surface too, or the ring renders untextured
        // around a textured home section -- a seam that moves with the camera.
#if AVER_MODULE_PBR
        if (content && materials) applySurface(*tile.renderer, *content, *materials);
#endif
        landscapeRingTiles_.emplace(t, std::move(tile));
    }

    AVER_INFO("[Landscape] ring around tile ({}, {}): {} tile(s) resident, {} draws/tile, "
              "{} resident-node cap/tile", camTile.tx, camTile.ty, landscapeRingTiles_.size(),
              kMaxDrawsPerTile, kMaxResidentNodesPerTile);
}

void GameLandscape::draw(rhi::IDevice& device, const Vec3& eye, const Mat4& viewProj, f32 viewportHeightPx,
                          GameContent* content, pbr::MaterialSystem* materials) {
    if (!loaded_ || !landscapeRenderer_) return;
#if AVER_MODULE_PBR
    // Bind the level's terrain material the first frame the material system is ready. Level load
    // cannot be trusted to be late enough: MaterialSystem::ready() also needs its GPU side up, with no
    // guaranteed order against the load, so the test runs every frame and self-heals.
    if (content && materials && !landscapeMaterial_.empty() &&
        (!landscapeRenderer_->hasSurfaceBinding() || !landscapeUvTilingResolved_))
        applySurfaceToAll(&device, *content, *materials);
#else
    (void)content; (void)materials;
#endif

    landscape::SelectParams lp;
    lp.cameraCm[0] = eye.x; lp.cameraCm[1] = eye.y; lp.cameraCm[2] = eye.z;
    // The same projection scale trifactor::projScale uses, over this frame's real viewport height:
    // a mismatched scale refines terrain at the wrong distance without failing visibly.
    lp.projScale = viewportHeightPx / (2.0f * std::tan(radians(60.0f) * 0.5f));
    lp.useFrustum = true;
    f32 vpm[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) vpm[r * 4 + c] = viewProj.m[r][c];
    lp.frustum = landscape::Frustum::fromViewProj(vpm);
    lp.maxDraws = kMaxDrawsPerTile;

    landscape::SelectResult lsel;
    landscapeTree_.select(lp, lsel);

    // Sections carry their world position in every sample (OcLandData::worldAt folds originCm in), so
    // the transform draw() applies on top is identity.
    static const f32 kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    landscapeRenderer_->draw(device, landscapeData_, landscapeTree_, lsel, kIdentity, landscapeUvTilingCm_);

    // The ring shares the same per-tile budget, which keeps total draws within the renderer's ceiling.
    for (auto& kv : landscapeRingTiles_) {
        RingTile& tile = kv.second;
        if (!tile.renderer) continue;
        landscape::SelectResult rsel;
        tile.tree.select(lp, rsel);
        tile.renderer->draw(device, tile.data, tile.tree, rsel, kIdentity, landscapeUvTilingCm_);
    }
}

void GameLandscape::setOriginCm(f32 x, f32 y, f32 z) {
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
    }
    return touched;
}

// Writes the resident section back to path(). A sculpt is fully functional in memory without this;
// it's the one place edits actually reach disk.
bool GameLandscape::save(std::string* why) {
    if (!loaded_ || landscapePath_.empty()) return false;
    if (!fmt::saveOcLand(landscapePath_, landscapeData_, why)) return false;
    dirty_ = false;
    // Saving is the natural commit point for a sculpt, so it is where the collision snapshot
    // catches up with the heights -- see rebuildCollision's own comment on why this is not done
    // per stroke.
    rebuildCollision();
    return true;
}

} // namespace aver::game

#endif // AVER_MODULE_LANDSCAPE
