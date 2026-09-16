// Runtime side: level load and unload, sky, landscape and water application, chunk streaming, navigation for a level.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"

namespace aver {
#if AVER_MODULE_SYNAPSE
// Loads the navigation that belongs to a level, if it has any. An ABSENT file is the normal
// case -- every level until somebody bakes one -- so it clears rather than complains.
void SandboxApp::loadNavForLevel(Engine& e) {
    nav_ = fmt::OcNavData{};
    const std::string path = editor::navPathForLevel(levelPath_);
    std::string why;
    if (!path.empty() && fmt::loadOcNav(path, nav_, &why)) {
        AVER_INFO("[Editor] navigation loaded from {} ({}x{} cells)", path,
                  nav_.widthCells, nav_.heightCells);
    }
    rebuildNavOverlay(e);
}

#endif

void SandboxApp::setFocusCompile(bool b) { tools_.armCompile(b); }

void SandboxApp::setFocusReload(int frames) { if (frames > 0) tools_.armReload(frames); }

#if AVER_MODULE_LANDSCAPE
// Loads one .ocland section and builds its quadtree. Pure CPU -- the mesh cache is created
// lazily by draw(), so this needs no device and may run before one exists.
// `device` frees the section CURRENTLY resident (if any) through forgetAll before replacing it;
// pass nullptr only when none has been created yet.
// Returns whether a section is now resident, so a caller can distinguish "loaded" from "there is
// no terrain here" rather than reading landscapeLoaded_ back out.
bool SandboxApp::loadLandscape(rhi::IDevice* device, const std::string& path) {
    unloadLandscape(device);
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
    // The section is tile (0,0) of the ring now -- its outer rim borders a procedural neighbour
    // like any ring tile's does, needing the same generous floor. 2x the noise amplitude is the
    // mathematical bound on how much a ridged-fBm field can vary at all (TerrainNoise.hpp).
    landscapeTree_.widenRimSkirts(2.0f * landscapeNoiseParams_.amplitudeCm);
    landscapeTree_.resetHysteresis();
    landscapeRenderer_ =
        std::make_unique<landscape::LandscapeRenderer>(kLandscapeMaxResidentNodesPerTile);
    landscapeLoaded_ = true;
    landscapePath_ = path;
    landscapeDirty_ = false;
    AVER_INFO("[Landscape] '{}' loaded: {} node(s) across {} level(s), {}x{} samples",
              path, landscapeTree_.nodes().size(), landscapeTree_.levelCount(),
              landscapeData_.sampleCount, landscapeData_.sampleCount);
    return true;
}

// safe because the generator lives in chunkWorld_, which this object owns and destroys.
void SandboxApp::applyLandscapeToStreaming() {
#if AVER_MODULE_SCENE
    // Nothing streaming yet: setChunkStreamingEnabled wires the source itself when it opens, so
    // the common order (level loads terrain, streaming switched on afterwards) needs nothing here.
    if (!chunkWorld_) return;
    // ChunkWorld exposes settings() as CONST ONLY -- no supported way to swap a live generator's
    // height source, and adding one would widen that module's API just for the editor's
    // convenience. Restarting streaming re-opens through the same known path and re-generates only
    // chunks around the camera, also dropping every chunk generated against the OLD surface (a known gap around generatorVersion).
    AVER_INFO("[ChunkWorld] terrain changed under a live stream -- restarting it so scatter "
              "re-generates against the new surface");
    setChunkStreamingEnabled(false);
    setChunkStreamingEnabled(true);
#endif
}

// Gives the resident section a static collision body, so things can stand on the terrain.
// REBUILT WHOLE, not patched: Jolt's heightfield shape is immutable once created, and there is no
// partial update -- why this runs at LOAD and SAVE, not per brush stroke, which would pay for the
// whole conversion several times a second.
// THE CONSEQUENCE, stated rather than hidden: between sculpting and saving, what you see and what
// you collide with disagree.
void SandboxApp::rebuildLandscapeCollision() {
#if AVER_MODULE_PHYSICS
    if (landscapeBody_ >= 0) { aver_phys_remove_body(landscapeBody_); landscapeBody_ = -1; }
    if (!landscapeLoaded_ || !aver_phys_ready()) return;
    landscape::PhysicsHeightfield hf;
    if (!landscape::toPhysicsHeightfield(landscapeData_, hf)) {
        AVER_WARN("[Landscape] section is not internally consistent; no collision built");
        return;
    }
    landscapeBody_ = aver_phys_add_heightfield(hf.samples.data(), static_cast<i32>(hf.sampleCount),
                                               hf.spacingCm, hf.cornerCm[0], hf.cornerCm[1],
                                               hf.cornerCm[2]);
    // Deliberately NOT stamped with aver_phys_set_entity: terrain has no owning scene entity in
    // this engine at all, not merely one this call site forgot to look up. A ray landing on it
    // is a genuine "hit true, entity 0" -- something WAS hit, nothing owns it -- not a bug.
    if (landscapeBody_ >= 0)
        AVER_INFO("[Landscape] collision body #{} built ({}x{} samples)", landscapeBody_,
                  hf.sampleCount, hf.sampleCount);
    else
        AVER_WARN("[Landscape] physics refused the heightfield; terrain has no collision");
#endif
}

// Frees the resident section's meshes (when a device exists to free them through) and drops it,
// AND every ring tile around it -- they are tile (0,0)'s neighbours and outlive their reason to
// exist the moment (0,0) does.
void SandboxApp::unloadLandscape(rhi::IDevice* device) {
    if (landscapeRenderer_ && device) landscapeRenderer_->forgetAll(*device);
    landscapeRenderer_.reset();
#if AVER_MODULE_PHYSICS
    if (landscapeBody_ >= 0) { aver_phys_remove_body(landscapeBody_); landscapeBody_ = -1; }
#endif
    landscapeLoaded_ = false;
    landscapePath_.clear();
    landscapeData_ = fmt::OcLandData{};
    landscapeDirty_ = false;
    sculpting_ = false;
    sculptCursorValid_ = false;
    for (auto& kv : landscapeRingTiles_)
        if (kv.second.renderer && device) kv.second.renderer->forgetAll(*device);
    landscapeRingTiles_.clear();
    landscapeLastCameraTileValid_ = false;
}

// Resolves which .ocland a level is standing on, in this order:
//   1. --landscape <path>, an explicit override that always wins
//   2. the level's own LANDSCAPE record, resolved against the project's Content
//   3. the levelname.ocland convention
// Silent when none resolve: most levels have no terrain yet.
// (2) IS WHY THE FORMAT RECORD EXISTS: it was added alongside the editor's ability to draw an
// .ocland, but the two halves were never joined, so a level could declare its terrain and the
// editor would ignore it and go looking for a filename instead.
// `at` OVERRIDES THE SECTION'S OWN originCm, and doing it here -- in the data, once -- is what
// makes every consumer (renderer, sculpt raycast, height source, physics bridge) agree without
// being told about placement separately.
// Pushes the level's LANDSCAPE material into one landscape renderer, as an opaque binding.
// THE HOST DOES THE RESOLVING, the whole reason setSurfaceBinding takes bytes and a handle instead
// of a pbr:: type: Aver.Landscape.Renderer links Core, RHI and Aver.Landscape only.
void SandboxApp::applyLandscapeSurface(landscape::LandscapeRenderer& r) {
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
    if (landscapeMaterial_.empty()) return;
    pbr::MaterialSystem& ms = voxiRenderer_.materials();
    if (!ms.ready()) return;
    const pbr::MaterialHandle h = materialForSurface(landscapeMaterial_);
    if (!h) return;
    const pbr::MaterialConstants& mc = ms.constants(h);
    r.setSurfaceBinding(ms.bindingSet(h), &mc, sizeof(pbr::MaterialConstants));

    // AND THE TEXTURE SCALE, the other half of "apply the material" that was missing: neither
    // call site passed uvTilingCm, so every landscape drew at the compiled-in 1000cm default (ten-
    // metre tiles). M_forest_leaves_02 authors `PARAM uvTiling 150` for its two-metre Poly Haven
    // source; ten-metre tiles stretch it 6.7x past scale, exactly the pale, washed-out ground the
    // demo captures show.
    // WHY THIS READS uvTilesPerCm RATHER THAN THE DESC: MaterialSystem exposes constants(), not
    // MaterialDesc, and the packed block already carries the reciprocal -- no new coupling needed.
    // A NOTE ON uvTiling'S DOCUMENTED SCOPE: MaterialDesc calls it "read only under WorldAligned",
    // true OF THE SHADER (averSurfaceUV) -- the mesh builder is a second, equally valid consumer
    // that bakes the same number into UVs instead of projecting it.
    // AND IT MUST BE THE REAL MATERIAL'S NUMBER, NOT THE FALLBACK'S: constants() returns
    // fallbackConstants_ (uvTiling 200) for any handle not yet valid, and materialForSurface() can
    // create one on the very frame this runs -- taking the fallback silently would latch the
    // landscape at 200cm looking like a plausible number rather than a bug. Ask the library the
    // same question constants() asks, and retry until it says yes.
    if (pbr::MaterialLibrary::get().valid(h) && mc.uvTilesPerCm > 0.0f) {
        landscapeUvTilingCm_ = 1.0f / mc.uvTilesPerCm;
        landscapeUvTilingResolved_ = true;
    }
#else
    (void)r;
#endif
}

// Re-applies it to the authored section and every resident ring tile at once. Called after a
// level load, and after the material system becomes ready -- whichever happens second is the one
// that actually binds anything, and neither is reliably first.
void SandboxApp::applyLandscapeSurfaceToAll(rhi::IDevice* device) {
    const f32 wasTiling = landscapeUvTilingCm_;
    if (landscapeRenderer_) applyLandscapeSurface(*landscapeRenderer_);
    for (auto& kv : landscapeRingTiles_)
        if (kv.second.renderer) applyLandscapeSurface(*kv.second.renderer);

    // THE CACHED MESHES CARRY THE OLD SCALE, so eviction belongs here (compared once around the
    // whole sweep) rather than inside applyLandscapeSurface, which runs once per renderer and
    // would leave every ring tile holding UVs built at the previous tiling.
    // buildChunkMesh bakes uvTilingCm into a node's UVs and draw() caches the result, so nothing
    // resident picks up a change on its own. Fires at most once per level, and nodes rebuild
    // lazily on the next draw, as after a sculpt.
    // THIS BLOCK IS NORMALLY SILENT, AND THAT IS NOT A SIGN IT DID NOTHING: the first apply happens
    // before a single chunk mesh exists, so there's nothing to evict -- verified by instrumenting
    // draw() directly: it receives 150, not the 1000cm default.
    if (device && landscapeUvTilingCm_ != wasTiling) {
        if (landscapeRenderer_) landscapeRenderer_->forgetAll(*device);
        for (auto& kv : landscapeRingTiles_)
            if (kv.second.renderer) kv.second.renderer->forgetAll(*device);
        AVER_INFO("[Landscape] texture tiling {:.0f}cm per tile, from material '{}' "
                  "(was {:.0f}); resident nodes dropped to rebuild",
                  landscapeUvTilingCm_, landscapeMaterial_, wasTiling);
    }
}

void SandboxApp::loadLandscapeForLevel(rhi::IDevice* device, const std::string& levelPath,
                           const fmt::OcWorldData& w) {
    std::string path = landscapeCliOverride_;
    bool haveAt = false;
    f64 at[3] = {0, 0, 0};

    if (path.empty() && !w.landscapes.empty()) {
        const fmt::OcLandscapePlacement& lp = w.landscapes.front();
        if (w.landscapes.size() > 1)
            AVER_WARN("[Landscape] level declares {} LANDSCAPE sections; the editor holds one and "
                      "is using '{}'. Tiling several sections is not implemented.",
                      w.landscapes.size(), lp.name.empty() ? lp.section : lp.name);
        // Taken even when the section path below fails: the material is a property of the
        // level's terrain, not of which file the heights came from, and the .ocland fallback
        // convention still wants it.
        landscapeMaterial_ = lp.material;
        if (!lp.section.empty()) {
            const std::string content = project_.contentDir();
            path = content.empty() ? lp.section : content + "\\" + lp.section;
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
        // rather than nudged -- otherwise LOD selection and frustum culling would run against
        // where the section used to be.
        std::string why;
        if (landscapeTree_.build(landscapeData_, landscapeTree_.nodeQuads(), &why)) {
            landscapeTree_.widenRimSkirts(2.0f * landscapeNoiseParams_.amplitudeCm);
            landscapeTree_.resetHysteresis();
            if (landscapeRenderer_ && device) landscapeRenderer_->forgetAll(*device);
            // The section moved, so its ring-tile grid (centred on ITS centre) moved too --
            // whatever was resident was built against the old placement and no longer borders it
            // correctly. Simplest fix: drop the ring and let updateLandscapeRingTiles() resynthesize next frame.
            for (auto& kv : landscapeRingTiles_)
                if (kv.second.renderer && device) kv.second.renderer->forgetAll(*device);
            landscapeRingTiles_.clear();
            landscapeLastCameraTileValid_ = false;
            AVER_INFO("[Landscape] placed at ({:.0f}, {:.0f}, {:.0f}) by the level's LANDSCAPE record",
                      at[0], at[1], at[2]);
        } else {
            AVER_ERROR("[Landscape] could not rebuild after placement: {}", why);
        }
    }
    rebuildLandscapeCollision();
    applyLandscapeToStreaming();
    applyLandscapeSurfaceToAll(device);
}

// Keeps a small window of PROCEDURAL tiles resident around (cameraXCm, cameraYCm), so the terrain
// extends past the authored section's own rim. Cheap to call every frame -- real work only happens
// the frame the camera's OWN tile coordinate changes, far less often than once a frame.
// Tile (0, 0) -- the authored section -- is never touched here; this only manages the RING around it.
void SandboxApp::updateLandscapeRingTiles(rhi::IDevice* device, f32 cameraXCm, f32 cameraYCm) {
    if (!landscapeLoaded_) return;
    const f32 tileSizeCm = landscapeData_.extentCm();
    if (!(tileSizeCm > 0.0f)) return;
    const f32 centreX = landscapeData_.originCm[0] + tileSizeCm * 0.5f;
    const f32 centreY = landscapeData_.originCm[1] + tileSizeCm * 0.5f;

    const landscape::TileCoord camTile =
        landscape::tileAt(cameraXCm, cameraYCm, centreX, centreY, tileSizeCm);
    if (landscapeLastCameraTileValid_ && camTile == landscapeLastCameraTile_) return;
    landscapeLastCameraTile_ = camTile;
    landscapeLastCameraTileValid_ = true;

    // Which coordinates should be resident now -- a (2R+1)x(2R+1) window around the camera's own
    // tile, minus (0,0) itself (that is the home tile above, not a ring tile).
    std::vector<landscape::TileCoord> want;
    want.reserve(kLandscapeMaxSectionsResident);
    for (i32 dy = -kLandscapeRingRadius; dy <= kLandscapeRingRadius; ++dy)
        for (i32 dx = -kLandscapeRingRadius; dx <= kLandscapeRingRadius; ++dx) {
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
    // authored section -- it already validated against LandscapeTree::build's tiling rule, so a
    // ring tile built the same way is guaranteed to validate too.
    for (const landscape::TileCoord& t : want) {
        if (landscapeRingTiles_.find(t) != landscapeRingTiles_.end()) continue;
        LandscapeRingTile tile;
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
        // Every rim of a ring tile borders SOMETHING -- the home tile, or another ring tile --
        // never open air, so all four get the same generous floor the home tile's outer rim got in
        // loadLandscape() (see LandscapeTree::widenRimSkirts for why an inner-LOD skirt can't cover a cross-tree neighbour).
        tile.tree.widenRimSkirts(2.0f * landscapeNoiseParams_.amplitudeCm);
        tile.tree.resetHysteresis();
        tile.renderer =
            std::make_unique<landscape::LandscapeRenderer>(kLandscapeMaxResidentNodesPerTile);
        // A tile born mid-session has to be told the surface too, or the ring renders untextured
        // around a textured home section -- a seam that moves with the camera.
        applyLandscapeSurface(*tile.renderer);
        landscapeRingTiles_.emplace(t, std::move(tile));
    }

    AVER_INFO("[Landscape] ring around tile ({}, {}): {} tile(s) resident, {} draws/tile, "
              "{} resident-node cap/tile", camTile.tx, camTile.ty, landscapeRingTiles_.size(),
              kLandscapeMaxDrawsPerTile, kLandscapeMaxResidentNodesPerTile);
}

#endif

#if AVER_MODULE_FLUIDS
// Turns a level's WATER/WAVE records into an actual surface, and into the buoyancy plane under it.
// A LEVEL BEATS THE COMMAND LINE: --water was never authoring, just a switch to look at water at
// all, and a level with no WATER record behaves as before.
// IT ALSO BRINGS THE RENDERER UP, which startup only did when --water was given: a pool should not
// need a CLI flag to appear.
void SandboxApp::applyLevelWater(Engine& eng) {
    if (levelHeader_.waters.empty()) return;

    const fmt::OcWaterPlacement& wp = levelHeader_.waters.front();

    // ONE SURFACE DRAWN, and said out loud rather than discovered: WaterRenderer holds a single
    // level and wave set, so a second WATER record has nowhere to go until the renderer can hold
    // more than one. The format allows several; this consumer does not yet.
    // BEFORE the simulate branch below, which returns: warning after it meant a level whose first
    // record was simulated got no warning about its second record at all.
    if (levelHeader_.waters.size() > 1)
        AVER_WARN("[Water] the level declares {} WATER records; only '{}' is rendered",
                  levelHeader_.waters.size(), wp.name.empty() ? "unnamed" : wp.name);

#if AVER_FLUIDS_SIMULATED
    // A SIMULATED RECORD IS NOT A GERSTNER SURFACE, and taking both paths would draw two waters
    // in the same hole fighting over the same depth. So this returns rather than falling
    // through: the soft body IS the water for this record.
    if (wp.simulate) {
        if (wp.infinite) {
            // The one pairing the format carries but nothing can honour: a simulated volume is a
            // closed shell needing a size, an endless ocean has none to give it. Refused here
            // rather than in the parser: the format's job is to carry what was written.
            AVER_WARN("[Water] '{}' asks to be simulated but declares no bounds; a simulated "
                      "volume needs a size, so it is left analytic",
                      wp.name.empty() ? "unnamed" : wp.name);
        } else {
            // NO DESPAWN HERE any more. loadLevel always runs unloadLevel first, and that is
            // where teardown lives now -- it is the one site every path that ends a level goes
            // through, including File > New Level, which never calls this function at all.
            fluids::FluidVolumeDesc fd;
            // The shell fills the authored footprint, hanging BELOW the surface line rather than
            // straddling it: the body's centre is half its depth under `level`. Depth is the
            // shallower of a sensible pool depth and the footprint, so a puddle never gets a shell deeper than it is wide.
            const f32 halfX = static_cast<f32>(wp.boundsMax[0] - wp.boundsMin[0]) * 0.5f;
            const f32 halfY = static_cast<f32>(wp.boundsMax[1] - wp.boundsMin[1]) * 0.5f;
            const f32 halfZ = std::min(60.0f, std::min(halfX, halfY));
            fd.centreCm[0] = static_cast<f32>(wp.boundsMin[0] + wp.boundsMax[0]) * 0.5f;
            fd.centreCm[1] = static_cast<f32>(wp.boundsMin[1] + wp.boundsMax[1]) * 0.5f;
            fd.centreCm[2] = static_cast<f32>(wp.levelCm) - halfZ;
            fd.halfExtentCm[0] = halfX;
            fd.halfExtentCm[1] = halfY;
            fd.halfExtentCm[2] = halfZ;
            // OVERRIDES FluidVolumeDesc's own {8,8,4} default HERE, at the one call site that
            // spawns a player-visible fluid volume, rather than raising the struct's default: a
            // decorative puddle or a dozen small fountains should not inherit a heavier solver cost
            // measured for ONE specific 6x4m pool. 14x14 horizontal (~43x29cm cells, was 8x8/~75cm)
            // is the finer top face WaterPerf's baseline called for; Z stays at the struct's own 4,
            // per that struct's own reasoning that vertical detail is rarely camera-visible on a
            // shallow pool. Both frame cost and shell integrity (SoftBodyTest's
            // testPressureHoldsAShellUp was swept AT 8x8x4, so re-checked by hand here) were
            // verified before this number was kept.
            fd.subdivisions[0] = 14;
            fd.subdivisions[1] = 14;

            // The four solver knobs, applied ONLY when this record actually named one. wp's own
            // fields default to -1 ("not authored"), and fd's own FluidVolumeDesc defaults are
            // exactly what a level written before these tokens existed already gets -- so leaving
            // an unauthored field alone keeps an old .ocmap simulating identically to before.
            if (wp.compliance >= 0.0) fd.compliance = static_cast<f32>(wp.compliance);
            if (wp.damping    >= 0.0) fd.damping    = static_cast<f32>(wp.damping);
            if (wp.iterations >= 0)   fd.iterations = static_cast<u32>(wp.iterations);
            if (wp.pressure   >= 0.0) fd.pressure   = static_cast<f32>(wp.pressure);

            // THE MATERIAL LAYER, same "applied only when actually named" rule as the four knobs
            // above: a non-empty preset wins outright; otherwise density/viscosity each apply
            // independently against FluidPhysicsMaterial's own defaults. Left unset when the
            // record names none of the three, so an old .ocmap or raw-knobs-only WATER record is
            // unaffected. The precedence check against a hand-set fd.damping happens in
            // fluids::fluidResolvePhysicsMaterial, not here.
            if (!wp.preset.empty()) {
                if (auto mat = fluids::fluidPhysicsMaterialPreset(wp.preset)) {
                    fd.material = *mat;
                } else {
                    AVER_WARN("[Water] '{}' names unknown material preset '{}'; no material applied",
                              wp.name.empty() ? "unnamed" : wp.name, wp.preset);
                }
            } else if (wp.density >= 0.0 || wp.viscosity >= 0.0) {
                fluids::FluidPhysicsMaterial mat;   // struct defaults are water's own numbers
                if (wp.density   >= 0.0) mat.densityKgM3  = static_cast<f32>(wp.density);
                if (wp.viscosity >= 0.0) mat.viscosityPaS = static_cast<f32>(wp.viscosity);
                fd.material = mat;
            }

            // THE SURFACE MATERIAL, a different question entirely from the three lines above and
            // deliberately not folded in: those decide how the volume MOVES (mass, damping), this
            // decides how it LOOKS. Carried as a name to the draw site (see fluidSurfaceMaterial_).
            fluidWantSurfaceMaterial_ = wp.material;

            // LATCHED, NOT SPAWNED -- the fix for every simulated record logging "the fluid body
            // could not be created" at startup: onInit reaches this function BEFORE render
            // features come up, and FluidScene::spawn's first line is `if (!ready_) return 0;`.
            // Deferred rather than reordered, because loadLevel is ALSO reached from File > Open
            // Level and the project browser, both inside onRender with this frame's command list
            // already open. A single drain in onUpdate is the only placement correct from all
            // three. navLoadPending_ and projectRenderPending_ are the same shape for the same reason.
            fluidWantDesc_    = fd;
            fluidWantName_    = wp.name;
            fluidWantPending_ = true;
            // The buoyancy plane still comes from the authored level: things floating ON a
            // simulated volume aren't floating on its actual deformed surface (the solver exposes
            // no query for that), and a flat plane at the authored height is closer than no plane at all.
            const f32 normal[3]  = {0.0f, 0.0f, 1.0f};
            const f32 current[3] = {0.0f, 0.0f, 0.0f};
            aver_phys_set_water_plane(static_cast<f32>(wp.levelCm), normal, 1.0f, 0.5f, 0.05f, current);
            return;
        }
    }
#endif
    // The waves belonging to this surface: the ones that name it, plus the ones that name nothing
    // at all -- which the format defines as meaning the FIRST declared water, and this is it.
    fluids::GerstnerWave waves[fluids::kMaxGerstnerWaves];
    size_t n = 0;
    size_t skipped = 0;
    for (const fmt::OcGerstnerWave& gw : levelHeader_.waves) {
        if (!gw.water.empty() && gw.water != wp.name) continue;
        if (n >= fluids::kMaxGerstnerWaves) { ++skipped; continue; }
        // The one narrowing from the format's f64 to the runtime's f32, at the boundary, exactly
        // where OcScatterSpecies' own comment says such a narrowing belongs.
        waves[n].dirX         = static_cast<f32>(gw.dirX);
        waves[n].dirZ         = static_cast<f32>(gw.dirZ);
        waves[n].wavelengthCm = static_cast<f32>(gw.wavelengthCm);
        waves[n].amplitudeCm  = static_cast<f32>(gw.amplitudeCm);
        waves[n].steepness    = static_cast<f32>(gw.steepness);
        ++n;
    }
    if (skipped)
        AVER_WARN("[Water] '{}' declares {} waves; the renderer takes {} and the rest are dropped",
                  wp.name.empty() ? "unnamed" : wp.name, n + skipped, fluids::kMaxGerstnerWaves);

    if (!waterAttached_) {
        if (!waterRenderer_.init(*eng.device())) {
            AVER_ERROR("[Water] the level authored water, but the renderer is unavailable on this device");
            return;
        }
        eng.device()->addRenderFeature(&waterRenderer_);
        waterAttached_ = true;
        waterEnabled_ = true;
    }

    waterRenderer_.setWaterLevelCm(static_cast<f32>(wp.levelCm));
    // AND THE BOUNDS ACTUALLY REACH THE RENDERER. Without this the record's `bounds` clause was
    // parsed, logged as "(bounded)" and then dropped -- a pool a few metres across drew water
    // over the entire level, and the log line read as though it had worked.
    if (wp.infinite) {
        waterRenderer_.clearWaterBounds();
    } else {
        waterRenderer_.setWaterBoundsCm(static_cast<f32>(wp.boundsMin[0]), static_cast<f32>(wp.boundsMin[1]),
                                        static_cast<f32>(wp.boundsMax[0]), static_cast<f32>(wp.boundsMax[1]));
    }
    // ZERO WAVES IS A LEGAL ANSWER, not a reason to fall back on the startup swell: a level that
    // declared a WATER record and no WAVEs asked for still water, and a pool usually wants exactly
    // that. gerstnerHeightCm's own contract already returns the flat level for an empty set.
    waterRenderer_.setWaves(waves, n);

    // THE SURFACE RIPPLE SET, different from the Gerstner swell above: the swell displaces
    // vertices on an analytic ocean, this shapes the NORMAL, read by the material graph and the
    // caustics, which must agree (IDevice::setWaterWaves).
    // DERIVED FROM THE AUTHORED WAVES WHERE THERE ARE ANY, else a default sized for a pool.
    // Non-harmonic wavelengths (37/23/61cm) avoid a visible beat, and each speed is a multiple of
    // 2*pi/3600 so it crosses gTime's hourly wrap without a jump.
    {
        f32 rip[3][4];
        if (n > 0) {
            for (u32 i = 0; i < 3; ++i) {
                const fluids::GerstnerWave& g = waves[i < n ? i : n - 1];
                const f32 len = std::sqrt(g.dirX * g.dirX + g.dirZ * g.dirZ);
                const f32 k = 6.2831853f / (g.wavelengthCm > 1.0f ? g.wavelengthCm : 1.0f);
                rip[i][0] = len > 1e-4f ? g.dirX / len : 1.0f;
                rip[i][1] = len > 1e-4f ? g.dirZ / len : 0.0f;
                rip[i][2] = k;
                // Deep-water dispersion, snapped to the hourly wrap: omega = sqrt(g*k).
                const f32 omega = std::sqrt(981.0f * k);
                rip[i][3] = std::round(omega / 0.001745329f) * 0.001745329f;
            }
        } else {
            const f32 kk[3] = {0.169816f, 0.273182f, 0.145670f};
            const f32 ss[3] = {3.740140f, 4.640800f, 2.879793f};
            const f32 dx[3] = {1.0f, 0.0f, 0.7071068f};
            const f32 dy[3] = {0.0f, 1.0f, 0.7071068f};
            for (u32 i = 0; i < 3; ++i) {
                rip[i][0] = dx[i]; rip[i][1] = dy[i]; rip[i][2] = kk[i]; rip[i][3] = ss[i];
            }
        }
        eng.device()->setWaterWaves(rip, 3, 0.055f);
    }

    waterHeightCm_ = static_cast<f32>(wp.levelCm);

    // The same single number for both, for the reason the startup path states: two independent
    // heights would drift, reading as broken buoyancy rather than a mismatch. Physics-guarded for
    // the same reason: a level may author water in a build with no solver to float anything.
#if AVER_MODULE_PHYSICS
    const f32 normal[3]  = {0.0f, 0.0f, 1.0f};
    const f32 current[3] = {0.0f, 0.0f, 0.0f};
    aver_phys_set_water_plane(waterRenderer_.waterLevelCm(), normal, 1.0f, 0.5f, 0.05f, current);
#endif

    AVER_INFO("[Water] level surface '{}' at z = {} cm with {} wave(s){}",
              wp.name.empty() ? "unnamed" : wp.name, wp.levelCm, n,
              wp.infinite ? "" : " (bounded)");
}

#endif

#if AVER_MODULE_SCENE
#if AVER_MODULE_SCENE
// True when any resident density field owns `e`. The World Outliner and the save path both use
// this to tell streamed entities from authored ones, so it MUST see every world -- a streamed
// entity that no world claims would be offered for editing and written into the level file.
bool SandboxApp::anyChunkWorldOwns(scene::Entity e) const {
    if (chunkWorld_ && chunkWorld_->owns(e)) return true;
    for (const auto& extra : chunkWorldsExtra_)
        if (extra && extra->owns(e)) return true;
    return false;
}

#endif
#endif

#if AVER_MODULE_SCENE
void SandboxApp::setChunkStreamingEnabled(bool on) {
    if (on == (chunkWorld_ != nullptr)) return;

    if (!on) {
        std::vector<i32> freed;
        world::StreamStats last = chunkWorld_->stats();
        chunkWorld_->shutdown(scene::World::instance(), freed);
        // Every additional field is torn down in the same pass and into the SAME `freed` list --
        // those bodies are as real as the primary's, and leaving them would leak a physics body
        // per streamed collider each time streaming is toggled.
        for (auto& extra : chunkWorldsExtra_) {
            if (!extra) continue;
            accumulateStreamStats(last, extra->stats());
            extra->shutdown(scene::World::instance(), freed);
        }
        chunkWorldsExtra_.clear();
        scene::World::instance().flush();
#if AVER_MODULE_PHYSICS
        for (const i32 b : freed) if (b >= 0) aver_phys_remove_body(b);
#endif
        chunkWorld_.reset();
        chunkStreamHaveLastPos_ = false;
        chunkStreamStats_ = world::StreamStats{};
        AVER_INFO("[ChunkWorld] streaming disabled -- {} chunk(s) / {} entities released",
                  last.residentChunks, last.residentEntities);
        return;
    }

    if (!project_.valid()) {
        AVER_WARN("[ChunkWorld] cannot enable streaming: no project is open");
        return;
    }

    // ---- ONE ChunkWorld PER DECLARED DENSITY FIELD ----
    // A ChunkWorld's loadRadius decides how far the world populates, and one radius cannot serve a
    // dense floor and a sparse canopy: at the shipped 3 the scatter ends 48m out while the camera
    // sees to the horizon, and raising it quadruples ground-cover chunks invisible at that distance.
    // Several fields, each with its own radius/seed/feature-size/species, is the fix: canopy at
    // radius 10 with 3 samples/axis, floor at 3 with 12, so cost scales with what's visible.
    // ONE WORLD PER FIELD rather than one generator holding several specs, since that's the
    // SMALLER change -- ChunkGenerator's sampling core is reproducibility-pinned.
    std::vector<const fmt::OcPcgVolume*> fields;
    for (const fmt::OcPcgVolume& pv : levelPcgVolumes_)
        if (pv.name != "Sky") fields.push_back(&pv);

    // No non-Sky volume still builds exactly ONE world on GeneratorSettings' shipped defaults --
    // the behaviour before any of this, and what the single-species cube fallback relies on.
    const usize fieldCount = fields.empty() ? usize{1} : fields.size();
    const bool multi = fieldCount > 1;

    // A species naming a volume the level does not declare would otherwise scatter nowhere, in
    // silence. Reported once per bad name and folded into the primary rather than dropped: a
    // typo in one SCATTER line must not delete that species from the world.
    for (const fmt::OcScatterSpecies& sp : levelHeader_.scatterSpecies) {
        if (sp.volume.empty()) continue;
        bool found = false;
        for (const fmt::OcPcgVolume* pv : fields) if (pv->name == sp.volume) { found = true; break; }
        if (!found)
            AVER_WARN("[ChunkWorld] SCATTER '{}' names volume '{}', which this level does not "
                      "declare; it will scatter in the first field instead",
                      sp.meshPath, sp.volume);
    }

    std::vector<std::unique_ptr<world::ChunkWorld>> built;
    for (usize fi = 0; fi < fieldCount; ++fi) {
        const fmt::OcPcgVolume* v = fields.empty() ? nullptr : fields[fi];

        // Which species this field places. An UNNAMED species goes to the first field, exactly
        // where every species went before the `volume` token existed, so a level never mentioning
        // volumes still produces one world with the whole palette.
        std::vector<fmt::OcScatterSpecies> mine;
        for (const fmt::OcScatterSpecies& sp : levelHeader_.scatterSpecies) {
            bool named = false;
            if (!sp.volume.empty())
                for (const fmt::OcPcgVolume* pv : fields) if (pv->name == sp.volume) { named = true; break; }
            if (named) { if (v && sp.volume == v->name) mine.push_back(sp); }
            else if (fi == 0)                            mine.push_back(sp);
        }
        // A field with no species would generate the fallback cube everywhere. Skip it: a level
        // may declare a field for something other than scatter (a cave mask, a moisture map).
        if (mine.empty()) {
            AVER_INFO("[ChunkWorld] field '{}' has no SCATTER species; not streamed",
                      v ? v->name : std::string("<none>"));
            continue;
        }

        auto cw = std::make_unique<world::ChunkWorld>();
        world::ChunkWorldSettings cwSettings;

        // Beside Content and Binaries, not inside either: generated/streamed state, not authored
        // content, so it must never appear in the Content Browser or be packaged as an asset.
        // PER FIELD ONLY WHEN THERE IS MORE THAN ONE: region files are keyed by chunk coordinate,
        // so two worlds sharing a directory would write each other's chunks. A single-field level keeps the plain "Chunks" path.
        cwSettings.worldDir = project_.dir + "\\Chunks";
        if (multi) cwSettings.worldDir += "\\" + (v && !v->name.empty() ? v->name
                                                                        : std::to_string(fi));

        // The scatter palette comes from the LEVEL's own SCATTER records; the editor has no
        // opinion about what a world scatters. A field whose species all fail validation streams
        // nothing rather than falling back to the cube.
        {
            std::vector<std::string> scatterErrors;
            if (!world::buildScatterPalette(mine, project_.contentDir(),
                                            cwSettings.generator.palette, scatterErrors)) {
                for (const std::string& e : scatterErrors)
                    AVER_WARN("[ChunkWorld] {}", e);
            }
        }

        // The field's own noise parameters. COVERAGE MAPS TO threshold, NOT to the generator's
        // pcg::InfiniteSpec::coverageFloor/coverageBias -- those are pinned to 0/1 in
        // GeneratedChunkSource::setSettings, because sampleInfinite's pow(remapped, bias) is the
        // one operation IEEE 754 does not pin across libm implementations, and this generator's reproducibility depends on bias always being 1.
        if (v) {
            cwSettings.generator.worldSeed = static_cast<u64>(static_cast<u32>(v->seed));
            if (v->cellSizeCm > 0.0) cwSettings.generator.featureSizeCm = static_cast<f32>(v->cellSizeCm);
            if (v->octaves > 0) cwSettings.generator.octaves = static_cast<u32>(v->octaves);
            {
                const f64 t = v->coverageFloor < 0.0 ? 0.0
                                                      : (v->coverageFloor > 1.0 ? 1.0 : v->coverageFloor);
                cwSettings.generator.threshold = static_cast<f32>(t);
            }
            // Clamped rather than trusted: cost is quadratic in this and the file is authored by
            // hand, so a stray digit would generate millions of entities per chunk.
            if (v->samplesPerAxis > 0) {
                constexpr i32 kMaxSamplesPerAxis = 64;   // 4096 candidates in one chunk
                const i32 n = v->samplesPerAxis > kMaxSamplesPerAxis ? kMaxSamplesPerAxis
                                                                      : v->samplesPerAxis;
                if (n != v->samplesPerAxis)
                    AVER_WARN("[ChunkWorld] PCGVOLUME '{}' asks for {} samples per axis; clamped to {}",
                              v->name, v->samplesPerAxis, n);
                cwSettings.generator.samplesPerAxis = static_cast<u32>(n);
            }
            // How far this field streams. Clamped for the same reason, and evictRadius is raised
            // with it: ChunkStreamer.hpp requires evictRadius > loadRadius or the boundary
            // thrashes, and it enforces that rather than trusting the caller.
            if (v->radiusChunks > 0) {
                constexpr i32 kMaxRadiusChunks = 24;
                const i32 r = v->radiusChunks > kMaxRadiusChunks ? kMaxRadiusChunks : v->radiusChunks;
                if (r != v->radiusChunks)
                    AVER_WARN("[ChunkWorld] PCGVOLUME '{}' asks for radius {}; clamped to {}",
                              v->name, v->radiusChunks, r);
                cwSettings.stream.loadRadius  = r;
                cwSettings.stream.evictRadius = r + 2;
            }
        }

        // Scatter follows the terrain when a section is resident. The generator asks only "what
        // is the surface Z at (x, y)" and knows nothing about landscapes, so the editor (which
        // depends on both) closes this lambda. The authored section wins where it exists; past its rim this falls through to the same continuous noise the ring tiles use.
#if AVER_MODULE_LANDSCAPE
        if (landscapeLoaded_) {
            cwSettings.generator.heightSource = [this](f32 x, f32 y, f32& outZ) {
                if (landscape::surfaceHeightAt(landscapeData_, x, y, outZ)) return true;
                outZ = landscape::terrainHeightAt(x, y, landscapeNoiseParams_);
                return true;
            };
        }
#endif

        world::RestoreOptions& restore = cw->streamer().restoreOptions();
#if AVER_MODULE_PBR
        restore.bindMaterial = [this](i32 token, const std::string& surface) {
            const pbr::MaterialHandle h = materialForSurface(surface);
            if (h) surfaceMaterials_[token] = h;
        };
#endif
#if AVER_MODULE_PHYSICS
        restore.createBody = [](scene::Entity e, const Vec3& worldPos, const Vec3& halfExtentCm) -> i32 {
            if (!aver_phys_ready()) return -1;
            const i32 body = aver_phys_add_static_box(worldPos.x, worldPos.y, worldPos.z,
                                                      halfExtentCm.x, halfExtentCm.y, halfExtentCm.z);
            if (body) aver_phys_set_entity(body, static_cast<i32>(e));
            return body;
        };
#endif

        std::string why;
        if (!cw->open(cwSettings, &why)) {
            // One field failing must not take the others down with it -- a level with a good
            // floor and a broken canopy should still show its floor.
            AVER_WARN("[ChunkWorld] field '{}' failed to open: {}",
                      v ? v->name : std::string("<none>"), why);
            continue;
        }

        AVER_INFO("[ChunkWorld] field '{}' -- worldDir='{}' loadRadius={} evictRadius={} "
                  "palette={} species threshold={:.2f} samples={}/axis (populated to {:.0f}m)",
                  v ? v->name : std::string("<none>"), cwSettings.worldDir,
                  cwSettings.stream.loadRadius, cwSettings.stream.evictRadius,
                  cwSettings.generator.palette.size(), cwSettings.generator.threshold,
                  cwSettings.generator.samplesPerAxis,
                  static_cast<f32>(cwSettings.stream.loadRadius * cwSettings.stream.chunkSizeCm) / 100.0f);
        built.push_back(std::move(cw));
    }

    if (built.empty()) {
        AVER_WARN("[ChunkWorld] cannot enable streaming: no density field produced a world");
        return;
    }

    chunkWorld_ = std::move(built[0]);
    chunkWorldsExtra_.clear();
    for (usize k = 1; k < built.size(); ++k) chunkWorldsExtra_.push_back(std::move(built[k]));

    chunkStreamHaveLastPos_ = false;
    chunkStreamLogsLeft_ = 8;
    chunkStreamStats_ = world::StreamStats{};
    AVER_INFO("[ChunkWorld] streaming enabled -- {} density field(s), chunkSize={}cm "
              "verticalRadius={}",
              built.size(), chunkWorld_->settings().stream.chunkSizeCm,
              chunkWorld_->settings().stream.verticalRadius);
    warnIfCameraOutsideGeneratedBand();
}

// Says so when streaming is switched on somewhere nothing will ever load.
// The generator fills a SINGLE BAND of chunk layers, and a camera above or below it gets an empty
// wanted-set by construction: no error, no chunks, a panel reading zero indistinguishable from
// "still starting up". Not hypothetical: ElectricDreams' 30000x30000 ground plane parks the
// level-load camera near Z=37000 while the default band reaches -1600..+3200.
// A WARNING, NOT A CORRECTION: moving the camera would be worse -- "the tool teleported me" is a
// harder bug to understand than "the tool told me I was out of range".
void SandboxApp::warnIfCameraOutsideGeneratedBand() const {
    if (!chunkWorld_) return;
    const world::StreamSettings& st = chunkWorld_->settings().stream;
    if (st.chunkSizeCm <= 0) return;

    const i32 camChunkZ = world::floorDiv(static_cast<i32>(camPos_.z), st.chunkSizeCm);
    const i32 surfaceZ  = chunkWorld_->settings().generator.surfaceChunkZ;
    if (std::abs(camChunkZ - surfaceZ) <= st.verticalRadius) return;

    // Inclusive of the top layer's full height, so the number quoted is the last Z that can
    // actually contain something rather than the coordinate its floor sits at.
    const i64 lo = i64(surfaceZ - st.verticalRadius) * st.chunkSizeCm;
    const i64 hi = i64(surfaceZ + st.verticalRadius + 1) * st.chunkSizeCm;
    AVER_WARN("[ChunkWorld] the camera is at Z={:.0f}cm (chunk layer {}), outside the generated "
              "band {}..{}cm (layers {}..{}). Nothing will load until it is inside that band -- "
              "press F to focus something near ground level, or fly down.",
              camPos_.z, camChunkZ, lo, hi, surfaceZ - st.verticalRadius, surfaceZ + st.verticalRadius);
}

// Spawns (or despawns) the graph-driven drone. OPT-IN, same shape as setChunkStreamingEnabled:
// Window > Drone or --drone, nothing touched until asked for.
// TRANSIENT, LIKE A CHUNK-STREAMED ENTITY, ON PURPOSE: droneEntity_ is never pushed to
// levelEntities_, so saveLevel/undo/redo never see it, and buildPanels' World Outliner filters it
// out explicitly by entity id, the same way it filters chunkWorld_->owns(e).
void SandboxApp::setDroneEnabled(bool on) {
    if (on == (droneEntity_ != scene::kInvalidEntity)) return;

    if (!on) {
        scene::World& world = scene::World::instance();
        if (world.valid(droneEntity_)) { world.destroy(droneEntity_); world.flush(); }
#if AVER_MODULE_SCRIPTING
        if (scripts_.ready()) scripts_.graphUnload(static_cast<i32>(droneEntity_));
#endif
        AVER_INFO("[Drone] disabled -- entity #{} released", (u32)droneEntity_);
        droneEntity_ = scene::kInvalidEntity;
        droneGraphLoaded_ = false;
        droneHaveLastPos_ = false;
        return;
    }

    if (!project_.valid()) {
        AVER_WARN("[Drone] cannot enable: no project is open");
        return;
    }
#if AVER_MODULE_SCRIPTING
    if (!scripts_.ready()) {
        AVER_WARN("[Drone] cannot enable: the scripting host is not running ({})",
                  scripts_.declineReason());
        return;
    }
    if (!scripts_.graphAvailable()) {
        AVER_WARN("[Drone] cannot enable: this build's staged bridge exports no Graph entry "
                  "points -- rebuild with the .NET SDK present so Aver.Scripting.Bridge picks up "
                  "GraphLoad/GraphTick/GraphUnload");
        return;
    }
#else
    AVER_WARN("[Drone] cannot enable: this build has no scripting module (AVER_MODULE_SCRIPTING=OFF)");
    return;
#endif
    scene::World& world = scene::World::instance();
    Transform xf;
    // AT THE PLAYER START WHEN THE LEVEL HAS ONE: the drone is what flies when a project declares
    // no GameMode, so it IS the player for that session, and that's exactly what a Player Start
    // says. Falling back to the camera keeps a level with no marker behaving as before.
    {
        Vec3 sp{}; f32 sy = 0.0f;
        if (playerStartTransform(sp, sy)) {
            xf.position = sp;
            xf.rotation = quatFromEulerDeg(Vec3{0.0f, 0.0f, sy});
            AVER_INFO("[Drone] spawning at the level's Player Start ({:.0f}, {:.0f}, {:.0f})",
                      sp.x, sp.y, sp.z);
        } else {
            xf.position = camPos_ + camForward() * kAddDistance;
            xf.rotation = Quat{0, 0, 0, 1};
        }
    }
    // 50cm half-extent applied to a mesh normalised so its rotor-tip diagonal reach is exactly
    // 1.0 gives a HUB-to-hub diagonal span of 2*0.80*50cm = 80cm, a prop-tip-to-prop-tip diagonal
    // of 2*1.00*50cm = 100cm, and a straight footprint of about 2*0.7657*50cm ~= 76.6cm -- a
    // mid-size camera/mapping drone, not a car (400-500cm) or a toy (25-40cm hub-to-hub):
    // comfortably "drone-sized".
    // Reusing kEditorCubeHalf rather than a second size dial: the SAME "how big does a built-in primitive spawn" constant the cube used.
    xf.scale = Vec3{kEditorCubeHalf, kEditorCubeHalf, kEditorCubeHalf};

    // FROZEN, same as spawnCube: the entity name is the asset path the mesh resolver hashes. Was
    // "Meshes/cube.ocmesh" -- a graph-driven actor spawned as a literal unit cube, the entire "the
    // drone has a box" complaint appendDrone exists to fix (see GameContent.cpp's runtime copy).
    static const std::string kDroneAsset = "Meshes/drone.ocmesh";
    const scene::Entity e = world.create(kDroneAsset, scene::kInvalidEntity, xf);
    if (e == scene::kInvalidEntity) { AVER_WARN("[Drone] the world refused a new entity"); return; }
    if (auto* mr = static_cast<scene::CMeshRenderer*>(
            world.addComponent(e, scene::kComponentMeshRenderer))) {
        mr->mesh = fnv1a64(std::string_view(kDroneAsset));
        mr->flags |= scene::kMeshRendererVisible;
        // The unit CUBE's box, not the drone's -- left over from when kDroneAsset really was the
        // cube. appendDrone's flat, spread-out silhouette needs its own bounds, or the drone ends
        // up exempt from frustum culling in the wrong direction: a box roughly six times taller
        // than the mesh it bounds -- the same failure mode inverted (too loose instead of absent).
        mr->aabbMin[0] = mr->aabbMin[1] = -0.78f; mr->aabbMin[2] = -0.22f;
        mr->aabbMax[0] = mr->aabbMax[1] =  0.78f; mr->aabbMax[2] =  0.16f;
        // A sensible built-in look for an unpainted quadcopter chassis: greyish and mostly metal
        // (the M_Metal entry in the surfaceLooks_ table, above) rather than the flat grey
        // 0.80/0.80/0.85 fallback an unset material draws.
        mr->material = aver_scene_material(0, "M_Metal");
    }
    // Deliberately NOT levelEntities_.push_back(e) and NOT pushEdit(...): see the comment above
    // this function for why.

#if AVER_MODULE_SCRIPTING
    // The graph comes from the PROJECT, named by --drone-graph; the engine has no opinion about
    // what it's called.
    // It used to be hardcoded as Content\Scripts\Drone.ocgraph, assuming every project contained
    // that file -- one sample's content compiled into the editor.
    // THE PROJECT'S OWN ANSWER FIRST, then the CLI override: until DRONE.GRAPH existed the drone
    // spawned with no graph from anywhere but --drone-graph, sitting perfectly still.
    const std::string& droneRel = !droneGraphRel_.empty() ? droneGraphRel_ : project_.droneGraph;
    const std::string graphPath =
        droneRel.empty() ? std::string() : project_.contentDir() + "\\" + droneRel;
    if (graphPath.empty()) {
        AVER_WARN("[Drone] entity #{} spawned with NO graph, so it will sit still. Give the "
                  "project a DRONE.GRAPH line (e.g. `DRONE.GRAPH Scripts/Drone.ocgraph`), or "
                  "pass --drone-graph <path relative to Content>.",
                  (u32)e);
        droneGraphLoaded_ = false;
    } else {
        droneGraphLoaded_ = scripts_.graphLoad(static_cast<i32>(e), graphPath);
        if (!droneGraphLoaded_)
            AVER_WARN("[Drone] entity #{} spawned but its graph would not load from '{}' -- see "
                      "the [Graph] error line just above for why", (u32)e, graphPath);
    }
#endif
    droneEntity_ = e;
    droneTimeSeconds_ = 0.0f;
    droneHaveLastPos_ = false;
    droneLogsLeft_ = 30;
    AVER_INFO("[Drone] enabled -- entity #{} spawned at ({:.0f},{:.0f},{:.0f}), graph {}",
              (u32)e, xf.position.x, xf.position.y, xf.position.z,
              droneGraphLoaded_ ? "loaded" : "NOT loaded");
}

// Sum of triangle counts over every entity chunkWorld_ currently owns. O(residentEntities),
// walked fresh each call rather than kept running -- a few hundred at most, and this only runs
// while the streaming panel is open or a log line needs it.
u64 SandboxApp::residentTriangleCount() const {
    u64 total = 0;
    if (!chunkWorld_) return total;
    const scene::World& world = scene::World::instance();
    // Every field, not just the primary: a canopy streaming at radius 10 is exactly the triangles
    // someone reading this number is trying to account for.
    const auto add = [&](const world::ChunkWorld& cw) {
        for (const scene::Entity e : cw.streamedEntities()) {
            const auto* mr = world.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
            if (!mr) continue;
            const auto it = meshTris_.find(mr->mesh);
            if (it != meshTris_.end()) total += it->second;
        }
    };
    add(*chunkWorld_);
    for (const auto& extra : chunkWorldsExtra_) if (extra) add(*extra);
    return total;
}

// Loads an .ocworld into the world as ordinary scene entities: transform, mesh and name.
// TAKES Engine& (it did not before) so it can hand a real device down to loadLandscapeForLevel:
// unloadLevel/unloadLandscape need a device to free what the PREVIOUS level left resident, and the
// only device this function ever has is the one its own two callers already hold.
void SandboxApp::loadLevel(Engine& eng, const std::string& path) {
    unloadLevel(eng);

    // DISPATCH ON WHAT THE FILE ACTUALLY USES, NOT ITS EXTENSION OR HEADER LINE: ElectricDreams
    // and FirstPerson both ship an .ocmap starting with `OCMAP 1` that is nonetheless pure OCWORLD
    // content, while OpenConstructor's demoworld.ocmap genuinely needs the legacy grammar. Without
    // this, every level went through fmt::loadOcworld, which "succeeds" on legacy OCMAP while
    // silently skipping ROOT/CLIENT/SURFACE/GROUND/KILLZ/DEFORM -- opening and saving
    // demoworld.ocmap destroyed all six records with no warning.
    if (fmt::levelFileIsLegacyOcmap(path)) {
        loadLegacyOcmapLevel(eng, path);
        return;
    }

    fmt::OcWorldData w;
    std::string why;
    if (!fmt::loadOcworld(path, w, &why)) { AVER_WARN("[Level] {}", why); return; }


    // CARRIED, NOT UNDERSTOOD. The editor has no UI for a PCGVOLUME and doesn't need one, but
    // saveLevel builds a fresh OcWorldData from the editor's own state, so anything it doesn't
    // hold is GONE on the next save. That silently deleted every PCGVOLUME in the level, including
    // the sky field and the forest generator's own: open, save, and the record no longer exists.
    // The same reasoning as the project manifest keeping unknown keys: preserve what you don't understand.
    levelPcgVolumes_ = w.pcgVolumes;
    // ...and the rest of the header, for exactly the same reason. Placements and PCG volumes are
    // stripped because they are already held elsewhere; what is left is identity, BUILD, ALGO,
    // SPAWN and the environment numbers the editor does not expose.
    levelHeader_ = w;
    levelHeader_.placements.clear();
    levelHeader_.pcgVolumes.clear();
    if (!levelPcgVolumes_.empty())
        AVER_INFO("[Level] carrying {} PCGVOLUME record(s) through the editor unchanged",
                  levelPcgVolumes_.size());

#if AVER_MODULE_FLUIDS
    // AND THE LEVEL'S OWN WATER, if it authored any. Placed here rather than beside the PCGVOLUME
    // carry above because this is the first point at which levelHeader_ holds the WATER/WAVE
    // records the file declared.
    applyLevelWater(eng);
#endif

    // AND NOW THE SKY FIELD ACTUALLY REACHES THE CLOUD LAYER, as it already did in the packaged
    // runtime: carrying the record through a save was all the editor ever did with it, so every
    // new project (scaffolded with `PCGVOLUME name Sky`) opened onto a bare gradient.
    // BY NAME, not "the first field": a level may declare a cave mask or moisture field too.
    // The floor is INVERTED into coverage: a HIGH density floor leaves LESS material standing, so
    // passing it through unchanged would clear the sky exactly when the author asked for overcast.
    for (const fmt::OcPcgVolume& v : levelPcgVolumes_) {
        if (v.name != "Sky") continue;
        const f64 floorV = v.coverageFloor < 0.0 ? 0.0 : (v.coverageFloor > 1.0 ? 1.0 : v.coverageFloor);
        sky_.cloudsEnabled = true;
        sky_.cloudSeed     = v.seed;
        sky_.cloudCoverage = static_cast<f32>(1.0 - floorV);
        AVER_INFO("[Level] sky field '{}' drives the cloud layer: seed {}, coverage {:.2f}",
                  v.name, sky_.cloudSeed, sky_.cloudCoverage);
        break;
    }

    // The placement loop is aver::world::instantiate now, shared with the game runtime. What is
    // left here is genuinely the EDITOR's: its label table, its entity->body map, and the
    // per-entity record saveLevel needs.
    // TERRAIN FIRST, THEN THE THINGS THAT STAND ON IT: this used to run at the end of loadLevel, harmless only while the ground was a flat plane -- a `snap` placement asks the ground how high it is.
#if AVER_MODULE_LANDSCAPE
    loadLandscapeForLevel(eng.device(), path, w);
#endif

    world::InstantiateOptions opt;
#if AVER_MODULE_LANDSCAPE
    // The same surface the scatter follows, so a hand-placed tree and a scattered fern sitting
    // a metre apart agree about where the ground is.
    opt.groundHeightAt = [this](f64 x, f64 y, f64& outZ) {
        if (!landscapeLoaded_) return false;
        f32 z = 0.0f;
        if (!landscape::surfaceHeightAt(landscapeData_, static_cast<f32>(x),
                                        static_cast<f32>(y), z)) return false;
        outZ = static_cast<f64>(z);
        return true;
    };
#endif
#if AVER_MODULE_PBR
    opt.bindMaterial = [this](i32 token, const std::string& surface) {
        const pbr::MaterialHandle h = materialForSurface(surface);
        if (h) surfaceMaterials_[token] = h;
    };
#endif
    const world::LevelInstance inst = world::instantiate(w, opt);
    levelEntities_ = inst.entities;
#if AVER_MODULE_PHYSICS
    levelBodies_ = inst.bodies;
#endif
    for (usize k = 0; k < inst.entities.size(); ++k) {
        const scene::Entity e = inst.entities[k];
        const fmt::OcWorldPlacement& p = w.placements[inst.placementIndex[k]];
        entityLabels_[static_cast<u32>(e)] = makeEntityLabel(p.material, p.asset);
        // WHY `collide` IS REMEMBERED AND THE MATERIAL IS NOT: the surface survives on the entity
        // (CMeshRenderer::material), but `nocollide` has no component at all -- a load-time
        // instruction nothing records afterwards. Inferring it from entityBodies_ would be wrong:
        // a level opened before aver_phys_init has no bodies for ANY placement.
        entityCollide_[static_cast<u32>(e)] = p.collide;
        // AND `snap`, FOR THE SAME REASON AND A WORSE FAILURE. snapToGround has no component
        // either, but unlike nocollide it also changes what z MEANS: with it set, the authored z
        // is an offset ABOVE the terrain, and world::instantiate resolves it to ground + offset
        // before it ever reaches CLocal. So a save that rebuilt the placement from the live
        // transform wrote the resolved height back as if it were the offset AND dropped the
        // token -- baking the terrain into the file, on an open-and-save with no edits at all.
        // LevelClassSave.hpp already says exactly this and already handles it, for CLASS
        // placements only; this is the ordinary-placement half.
        if (p.snapToGround) entitySnapZ_[static_cast<u32>(e)] = p.z;
#if AVER_MODULE_PHYSICS
        if (inst.entityBody[k] >= 0) entityBodies_[static_cast<u32>(e)] = inst.entityBody[k];
#endif
    }

#if AVER_MODULE_FRAMEWORK
    // GRAPH-AS-CLASS / any other class placement -- collected here, SPAWNED LATER by
    // spawnClassPlacements(): applyProject's "Loading level" stage runs BEFORE "Starting scripts",
    // so a class declared from a .ocgraph isn't registered yet -- aver_fw_class_find would always miss it here.
    classPlacements_.clear();
    for (const fmt::OcWorldPlacement& p : w.placements)
        if (!p.className.empty()) classPlacements_.push_back(p);
#endif

    // A level that states where the player starts gets a visible, movable marker for it. Without
    // this the SPAWN record was invisible in the editor: authored only by hand-editing the file,
    // and impossible to see or move once written.
#if AVER_MODULE_SCENE
    playerStart_ = scene::kInvalidEntity;
    if (w.hasSpawn) {
        playerStart_ = makePlayerStart(Vec3{static_cast<f32>(w.spawnX), static_cast<f32>(w.spawnY),
                                             static_cast<f32>(w.spawnZ)},
                                        static_cast<f32>(w.spawnYaw));
    }
#endif

    // FOG USED TO BE UNPACKED HERE, four lines above the call that now does it. Two places
    // reading the same record is how they drift, and this pair already had: this block set
    // fogDensity_ while saveLevel wrote a different member entirely.
    applyLevelSky(w);
    levelPath_ = path;
    levelName_ = w.name;
#if AVER_MODULE_SYNAPSE
    // The overlay is a DEVICE resource and this function has no device -- loadLevel is
    // reachable from paths that pass no Engine at all. Latched here and serviced from the
    // frame loop, which is the only place a line mesh can be created.
    navLoadPending_ = true;
#endif

    sel_ = -1;
    selEntity_ = scene::kInvalidEntity;

    // A STORED VIEWPOINT OUTRANKS AUTO-FRAMING, because it is the more specific statement:
    // frameCameraOn guesses a view from the level's bounds, and a CAMERA record is where the
    // author actually was. Guessing is the fallback for a level that has never been saved with
    // one, which is every level written before the record existed.
    //
    // --cam STILL WINS, and it now does so BY ASKING rather than by ordering. The claim used to
    // be that the camOverride_ block in onInit runs after this one, which is true only for the
    // start map: --open-level is drained from the FRAME LOOP, long after onInit has applied the
    // override, so a level opened that way restored its own CAMERA record straight over it.
    // Measured: four captures at four different --cam positions returned the same probe
    // (72,71,79) and the same image, because none of the four cameras was ever used.
    //
    // A COMMAND LINE OUTRANKS STORED STATE. That is the same rule a project manifest already
    // learned the hard way when it silently outranked the render flags, and the failure has the
    // same shape both times: the run is not wrong, it just measures somewhere else, and nothing
    // says so. Refusing the restore here rather than re-applying the override afterwards keeps
    // one writer for the camera on this path instead of two that must stay in order.
    // BOTH WRITERS ARE SKIPPED, not just the restore: the frameCameraOn fallback below is the
    // other way this function moves the camera, and letting it run would replace one silent
    // override with another.
    if (camOverride_) {
        if (w.hasCamera) AVER_INFO("[Level] CAMERA record ignored -- --cam was given and outranks it");
    } else if (w.hasCamera) {
        constexpr f32 kRad = 3.14159265358979323846f / 180.0f;
        camPos_ = Vec3{static_cast<f32>(w.camX), static_cast<f32>(w.camY), static_cast<f32>(w.camZ)};
        yaw_    = static_cast<f32>(w.camYaw) * kRad;
        pitch_  = static_cast<f32>(w.camPitch) * kRad;
        // Clamped to the same limits the mouse-look path enforces, so a hand-edited or
        // corrupted file cannot put the camera somewhere the controls can never recover from.
        pitch_ = pitch_ < -1.54f ? -1.54f : (pitch_ > 1.54f ? 1.54f : pitch_);
        // 0 means UNSTATED -- a level that carried no speed leaves the user's preference alone
        // rather than resetting the camera to a stored zero and appearing to freeze.
        if (w.camSpeed > 0.0) {
            flySpeed_ = static_cast<f32>(w.camSpeed);
            flySpeed_ = flySpeed_ < 20.0f ? 20.0f : (flySpeed_ > 40000.0f ? 40000.0f : flySpeed_);
        }
        levelCameraRestored_ = true;
        // LOGGED because this is otherwise invisible: a restored camera and an auto-framed one
        // look the same from outside the process, and the difference is exactly what a person
        // reporting "it didn't remember where I was" needs to be able to check.
        AVER_INFO("[Level] camera restored to ({:.0f},{:.0f},{:.0f}) yaw {:.1f} pitch {:.1f} speed {:.0f}",
                  camPos_.x, camPos_.y, camPos_.z, w.camYaw, w.camPitch, flySpeed_);
    } else if (!w.placements.empty()) {
        frameCameraOn(w);
    }

#if AVER_MODULE_VOXI
    // OUTSIDE THE CAMERA CHAIN ABOVE, because fitting the GI volume used to happen at the bottom
    // of frameCameraOn -- which is only the fallback branch. A level with a saved CAMERA record,
    // or a run passing --cam, never fitted its volume at all: where the camera ends up and how
    // big the level is are different questions and only one of them is about the camera.
    //
    // BUT AN AUTHORED VOLUME STILL WINS, and that guard is not optional. RENDER.GIVOLUME is a
    // number a person chose; a fit computed from placement bounds is a guess, and a guess must
    // not silently replace authorship. I got this wrong first time round and measured the cost
    // on PTTest, which authors a 5659cm half-extent: fitting unconditionally REPLACED it with
    // 2356cm and shrank the volume for a level it already covered. project_.giExtent is 0 when
    // the manifest states nothing, which is exactly the "no author has an opinion" case the fit
    // is for.
    if (!w.placements.empty() && project_.giExtent <= 0.0f) fitGiVolumeTo(w);
#endif

    // JUST LOADED MEANS JUST SAVED, as far as the exit prompt is concerned. openLevelDirect
    // does NOT unload first, so without this a level opened after editing another would
    // inherit that one's history and be reported dirty the instant it appeared.
    markLevelSaved();
    AVER_INFO("[Level] '{}' loaded from {} ({} placement(s))", w.name, path, w.placements.size());

}

// Loads a LEGACY .ocmap -- one that actually USES at least one of ROOT/CLIENT/SURFACE/GROUND/
// KILLZ/DEFORM, so it has to load through fmt::loadOcmap, the only parser with a branch for any of
// them (loadOcworld above accepts the same header line without complaint but is the wrong loader).
// Split out from loadLevel rather than folded in as a mid-function branch: a legacy file has none
// of loadLevel's OCWORLD-only concerns, so reusing that function's body would mean guarding every
// section against a struct that can never carry them.
void SandboxApp::loadLegacyOcmapLevel(Engine& eng, const std::string& path) {
    (void)eng;   // unlike loadLevel, this path has no landscape/device work to hand it to
    fmt::OcMapData m;
    std::string why;
    if (!fmt::loadOcmap(path, m, &why)) { AVER_WARN("[Level] {}", why); return; }

    // CARRIED, NOT UNDERSTOOD -- see legacyMapHeader_'s own comment for which five record kinds
    // this is and why. saveLevelAsOcmap starts from this and overwrites only what the editor
    // genuinely owns, the identical levelHeader_ pattern the OCWORLD path above uses.
    legacyMapHeader_ = m;
    legacyMapHeader_.placements.clear();
    levelIsLegacyOcmap_ = true;

    // TRANSLATED INTO THE SHARED PLACEMENT PIPELINE, not re-implemented: world::instantiate
    // already does everything a placement needs, and OcPlacement/OcWorldPlacement already agree
    // field for field, so this is translation, not invention.
    // MATERIAL IS DELIBERATELY LEFT EMPTY on every synthesised placement: a legacy PLACE names a
    // numeric SURFACE-table index and a DEFORM a soft-body material like "rubber", neither an
    // .ocmat name -- feeding either to aver_scene_material would resolve a material that doesn't
    // exist rather than leaving the mesh's own cooked material in place. Both values are preserved
    // for the save through entityLegacySurface_/entityLegacyMaterial_ instead.
    fmt::OcWorldData synth;
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
        // `p.scale` is already forced to 1.0 for a DEFORM record (OcMap.cpp hardcodes it there;
        // legacy DEFORM has no scale concept of its own), so this one line is correct for both
        // placement kinds without a branch.
        op.sx = op.sy = op.sz = (p.scale == 0.0 ? 1.0 : p.scale);
        // EVERY legacy placement is a collision source in this editor's hands -- ocmapIsServerValid
        // itself requires GROUND or at least one PLACE for exactly that reason. A DEFORM's real
        // behaviour is a server-simulated soft body, not this static box, but nothing here spawns a
        // deformable cage, so a static box is the honest stand-in, no worse than an ordinary PLACE with scale 1.
        op.collide = true;
        synth.placements.push_back(std::move(op));
    }

    world::InstantiateOptions opt;
    const world::LevelInstance inst = world::instantiate(synth, opt);
    levelEntities_ = inst.entities;
#if AVER_MODULE_PHYSICS
    levelBodies_ = inst.bodies;
#endif
    for (usize k = 0; k < inst.entities.size(); ++k) {
        const scene::Entity e = inst.entities[k];
        const fmt::OcPlacement& p = m.placements[inst.placementIndex[k]];
        entityLabels_[static_cast<u32>(e)] = makeEntityLabel(std::string(), p.asset);
        entityCollide_[static_cast<u32>(e)] = true;
        // WHICH RECORD KIND THIS ENTITY CAME FROM, AND ITS OWN FIELD -- see this function's
        // "MATERIAL IS DELIBERATELY LEFT EMPTY" paragraph above for why these live here, and
        // entityLegacyDeform_'s own comment for what an entity absent from these maps saves as.
        entityLegacyDeform_[static_cast<u32>(e)] = p.deform;
        if (p.deform) entityLegacyMaterial_[static_cast<u32>(e)] = p.material;
        else          entityLegacySurface_[static_cast<u32>(e)] = p.surface;
#if AVER_MODULE_PHYSICS
        if (inst.entityBody[k] >= 0) entityBodies_[static_cast<u32>(e)] = inst.entityBody[k];
#endif
    }

#if AVER_MODULE_SCENE
    playerStart_ = scene::kInvalidEntity;
    if (m.hasSpawn) {
        playerStart_ = makePlayerStart(Vec3{static_cast<f32>(m.spawnX), static_cast<f32>(m.spawnY),
                                             static_cast<f32>(m.spawnZ)},
                                        static_cast<f32>(m.spawnYaw));
    }
#endif
    levelPath_ = path;
    levelName_ = m.name;

    sel_ = -1;
    selEntity_ = scene::kInvalidEntity;

    // The legacy .ocmap path fitted the GI volume through frameCameraOn's old side effect too,
    // so it gets the explicit call for the same reason the .ocworld path above does.
    if (!synth.placements.empty()) frameCameraOn(synth);
#if AVER_MODULE_VOXI
    if (!synth.placements.empty() && project_.giExtent <= 0.0f) fitGiVolumeTo(synth);
#endif

    AVER_INFO("[Level] '{}' loaded from {} ({} placement(s), legacy .ocmap)", m.name, path,
              m.placements.size());
}

#endif

#if AVER_MODULE_SCENE
#if AVER_MODULE_FRAMEWORK
// GRAPH-AS-CLASS / any other class placement: mirrors GameLevel::spawnClassPlacements. Spawned
// FOR REAL (aver_fw_spawn), not previewed -- there's no "inert until Play" precedent to build on,
// and this matches how every ORDINARY mesh placement already behaves. Accepted consequence: a
// placed class runs OnTick immediately on load, even outside Play.
void SandboxApp::spawnClassPlacements() {
    for (usize pi = 0; pi < classPlacements_.size(); ++pi) {
        const fmt::OcWorldPlacement& p = classPlacements_[pi];
        const int32_t c = aver_fw_class_find(p.className.c_str());
        if (c == 0) {
            AVER_WARN("[Level] placement names class '{}', which is not declared -- skipped", p.className);
            continue;
        }

        f64 pz = p.z;
#if AVER_MODULE_LANDSCAPE
        // Same ground query loadLevel's own opt.groundHeightAt uses -- not persisted from there
        // (opt is local to loadLevel), so re-expressed here rather than threaded through as a
        // member for one caller.
        if (p.snapToGround && landscapeLoaded_) {
            f32 gz = 0.0f;
            if (landscape::surfaceHeightAt(landscapeData_, static_cast<f32>(p.x),
                                           static_cast<f32>(p.y), gz))
                pz = static_cast<f64>(gz) + p.z;
        }
#endif
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
        levelClassInstances_.push_back(ClassInstance{pi, e});
    }
    if (!levelClassInstances_.empty())
        AVER_INFO("[Level] {} class instance(s) placed -- an entity exists for each; whether its graph "
                  "COMPILED is reported per instance above, because aver_fw_spawn returns a live entity "
                  "even when the managed bind behind it failed, so this count is placement, not success",
                  levelClassInstances_.size());
}

#endif
#endif

#if AVER_MODULE_SCENE
// Applies a level's SUN, SKY, FOG and CLOUDS records to the live atmosphere.
//
// The MAPPING is aver::assets::applyLevelEnv, shared with the shipped game -- see LevelSky.hpp
// for why it is one function and not two. What stays here is what is genuinely the EDITOR's:
// its own mirrors of the fields the Details sliders bind to, the record-present flags saveLevel
// reads, and the one warning that only makes sense to somebody authoring a level.
void SandboxApp::applyLevelSky(const fmt::OcWorldData& w) {
    assets::applyLevelEnv(w, sky_);

    // RESEEDED FROM THE ATMOSPHERE. The frame loop copies each of these back over sky_ every
    // frame (see the "FROZEN: sunDirection stays unnormalised" block), so applying a level and
    // not doing this would show its sky for exactly zero frames -- which is the shape the sun's
    // `lux` bug already had once: a level could state any intensity and every scene rendered at
    // the editor's default, silently, with a BIT-IDENTICAL frame either way.
    for (int i = 0; i < 3; ++i) {
        sunColor_[i]   = sky_.sunColor[i];
        skyZenith_[i]  = sky_.zenith[i];
        skyHorizon_[i] = sky_.horizon[i];
        fogColor_[i]   = sky_.fogColor[i];
    }
    sunAmbient_ = sky_.skyLightIntensity;
    fogDensity_ = sky_.fogDensity;

    if (w.hasSun) {
        // A sun below the horizon is legal -- the physical model renders night -- but under the
        // authored dome it silently lit everything from underneath, so no level was ever told.
        // Say it out loud rather than clamping: only the author knows if they meant it.
        f32 elev = 0.0f, azim = 0.0f;
        sky_.sunAngles(elev, azim);
        if (elev < 0.0f)
            AVER_WARN("[Level] SUN is {:.1f} degrees BELOW the horizon (dir {:.3f} {:.3f} {:.3f}). "
                      "The physical sky renders that as night; --sky-authored lights from below "
                      "as it always did.", elev, w.sunDir[0], w.sunDir[1], w.sunDir[2]);
        hasLevelSun_ = true;
    }
    if (w.hasSky)    hasLevelSky_    = true;
    if (w.hasFog)    hasLevelFog_    = true;
    if (w.hasClouds) hasLevelClouds_ = true;
}

// Puts the editor camera where the whole level is visible, and fits the fly speed and the GI
// volume to its bounds.
// The level's world-space bounds as a centre and a radius. Split out of frameCameraOn because
// TWO UNRELATED THINGS were reading it and only one of them is about the camera.
//
// WHAT THAT COUPLING COST. Fitting the GI volume lived at the bottom of frameCameraOn, and
// frameCameraOn only runs for a level with NO saved CAMERA record -- it is the fallback for
// "we do not know where to look". So saving a level's camera silently stopped its GI volume
// being fitted, and the volume stayed at its authored default (centre 0,0,300, half-extent
// 1200cm) no matter how big the level was. Measured on Intel Sponza, which spans X -1386..747,
// Y -1595..1949, Z -43..1688: a third of the level sat outside its own GI volume, and nothing
// said so. Adding --cam made it worse for the same reason -- skipping the camera framing also
// skipped the fit.
//
// A radius, not a box, because that is what both consumers want: the camera wants a distance to
// stand back by, and setVolume takes a half-edge extent.
void SandboxApp::levelBounds(const fmt::OcWorldData& w, Vec3& centre, f32& radius) const {
    // THE MESH BOX, SCALED AND ROTATED -- not p.sx/sy/sz used as a size. This loop read
    // fabs(p.sx/sy/sz) as a half-extent, but sx/sy/sz is a dimensionless SCALE MULTIPLIER
    // (Transform::scale, nothing converts it), so an ordinary PLACE (scale 1.0) contributed a 1cm
    // cube and this "bounds" was really the placement POSITIONS' point cloud.
    // It looked right by coincidence: PLACEG conventionally uses a unit-cube mesh, where a scale
    // and a half-extent are the same number.
    // Kept identical to GameLevel.cpp's placementBounds: editor preview and shipped game must fit
    // the same volume, and these two loops have already drifted once.
    static const Vec3 kCorner[8] = {{-1,-1,-1},{1,-1,-1},{-1,1,-1},{1,1,-1},
                                    {-1,-1, 1},{1,-1, 1},{-1,1, 1},{1,1, 1}};
    Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    for (const fmt::OcWorldPlacement& p : w.placements) {
        const Vec3 c{static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)};
        // An asset with no loaded bounds contributes its position only: it occupies no space we
        // can prove, and inventing one would let a single bad line inflate the whole volume.
        Vec3 mlo{0,0,0}, mhi{0,0,0};
        const auto itB = meshBounds_.find(fnv1a64(std::string_view(p.asset)));
        if (itB != meshBounds_.end()) { mlo = itB->second.first; mhi = itB->second.second; }
        const Vec3 mc{(mlo.x+mhi.x)*0.5f, (mlo.y+mhi.y)*0.5f, (mlo.z+mhi.z)*0.5f};
        const Vec3 mh{(mhi.x-mlo.x)*0.5f, (mhi.y-mlo.y)*0.5f, (mhi.z-mlo.z)*0.5f};
        const Quat rot = world::quatFromEulerDeg(Vec3{static_cast<f32>(p.roll),
                                                      static_cast<f32>(p.pitch),
                                                      static_cast<f32>(p.yaw)});
        for (const Vec3& k : kCorner) {
            const Vec3 local{(mc.x + k.x*mh.x) * static_cast<f32>(p.sx),
                             (mc.y + k.y*mh.y) * static_cast<f32>(p.sy),
                             (mc.z + k.z*mh.z) * static_cast<f32>(p.sz)};
            const Vec3 wpt = c + rot.rotate(local);
            lo.x = std::fmin(lo.x, wpt.x); hi.x = std::fmax(hi.x, wpt.x);
            lo.y = std::fmin(lo.y, wpt.y); hi.y = std::fmax(hi.y, wpt.y);
            lo.z = std::fmin(lo.z, wpt.z); hi.z = std::fmax(hi.z, wpt.z);
        }
    }
    // Every placement missing and the sentinels never moved: a level with no placements at all.
    // Guarded because the radius below would otherwise be computed from 1e9-(-1e9).
    if (w.placements.empty() || lo.x > hi.x) { lo = Vec3{0,0,0}; hi = Vec3{0,0,0}; }
    centre = Vec3{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    radius = std::fmax(1.0f, 0.5f * std::sqrt((hi.x-lo.x)*(hi.x-lo.x) +
                                              (hi.y-lo.y)*(hi.y-lo.y) +
                                              (hi.z-lo.z)*(hi.z-lo.z)));
}

#endif

#if AVER_MODULE_SCENE
#if AVER_MODULE_VOXI
// Fits the GI volume to a level. Called on EVERY level load, whatever the camera does -- see
// levelBounds for what happened while this was reachable only through the camera fallback.
void SandboxApp::fitGiVolumeTo(const fmt::OcWorldData& w) {
    Vec3 centre; f32 radius = 1.0f;
    levelBounds(w, centre, radius);
    giCenter_ = centre;
    giExtent_ = radius;
    AVER_INFO("[Voxi] GI volume fitted to the level: centre ({:.0f},{:.0f},{:.0f}) half-extent {:.0f}cm",
              centre.x, centre.y, centre.z, radius);
}

#endif
#endif

#if AVER_MODULE_SCENE
void SandboxApp::frameCameraOn(const fmt::OcWorldData& w) {
    Vec3 centre; f32 radius = 1.0f;
    levelBounds(w, centre, radius);
    const f32 dist = radius * 1.6f;
    camPos_ = Vec3{centre.x - dist * 0.65f, centre.y - dist * 0.65f, centre.z + dist * 0.55f};
    const Vec3 look = (centre - camPos_).getSafeNormal();
    yaw_   = std::atan2(look.y, look.x);
    pitch_ = std::asin(std::fmax(-1.0f, std::fmin(1.0f, look.z)));
    flySpeed_ = std::fmax(flySpeed_, radius * 0.02f);
    // This is a teleport, not a move: the next chunk-streaming update must not see this as a
    // (huge, one-frame) velocity computed against wherever the camera used to be.
    chunkStreamHaveLastPos_ = false;
}

void SandboxApp::loadStartMap(Engine& eng) {
    // A MAP NAMED ON THE COMMAND LINE OUTRANKS THE PROJECT'S START MAP, loaded even when the
    // project has no start map at all -- the whole point of naming one. Checked before
    // project_.valid(), so a level outside any project still opens rather than silently opening nothing.
    if (!openMapPath_.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(openMapPath_, ec)) { loadLevel(eng, openMapPath_); return; }
        AVER_ERROR("[Level] '{}' does not exist", openMapPath_);
        openMapPath_.clear();                  // fall back to the start map rather than nothing
    }
    if (!project_.valid() || project_.startMap.empty()) return;
    const std::string path = project_.contentDir() + "\\" + project_.startMap;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        AVER_INFO("[Level] start map '{}' does not exist yet - the world starts empty", project_.startMap);
        levelName_ = std::filesystem::path(project_.startMap).stem().string();
        levelPath_ = path;
        return;
    }
    loadLevel(eng, path);
}

// Destroys the loaded level's entities and everything keyed to them: labels, bodies, undo.
void SandboxApp::unloadLevel(Engine& eng) {
    (void)eng;   // only read under AVER_MODULE_LANDSCAPE, at the end of this function
#if AVER_MODULE_FRAMEWORK
    // BEFORE the raw-entity loop below, and through aver_fw_destroy rather than world.destroy() --
    // see GameLevel::unload's identical comment for why (the managed-dispatch unbind hook is what
    // releases a graph-class instance's GraphHost/VAR storage).
    for (const ClassInstance& ci : levelClassInstances_) aver_fw_destroy(ci.entity);
    levelClassInstances_.clear();
    classPlacements_.clear();
#endif
    scene::World& world = scene::World::instance();
    for (const scene::Entity e : levelEntities_) if (world.valid(e)) world.destroy(e);
    levelEntities_.clear();
#if AVER_MODULE_SCENE
    // THE PLAYERSTART MARKER needs its own line here for the reason it's not in levelEntities_:
    // deliberately transient, so saving doesn't emit it as a PLACE record. Consequence: loadLevel
    // reassigns playerStart_ on its way in, hiding this everywhere except File > New Level.
    // What that cost: the previous level's marker kept rendering in the new empty level, and
    // Toolbar > Add > Player Start SELECTED the stale entity instead of creating a new one.
    // Same shape, same place, same reasoning as the simulated fluid volume torn down further down:
    // anything a level owns but levelEntities_ doesn't hold must be released here by name.
    if (playerStart_ != scene::kInvalidEntity) {
        if (world.valid(playerStart_)) world.destroy(playerStart_);
        playerStart_ = scene::kInvalidEntity;
    }
    playerStartYaw_ = 0.0f;
#endif
    entityLabels_.clear();
    labelCounts_.clear();
    entityBodies_.clear();
    // Cleared with the rest of the level's state: carrying one level's PCG records into the
    // next would write them into a file that never had them.
    levelPcgVolumes_.clear();
    levelHeader_ = fmt::OcWorldData{};
    entityCollide_.clear();
    entitySnapZ_.clear();
    // The legacy OCMAP state, cleared with the rest -- see levelIsLegacyOcmap_'s own comment
    // for why a stale `true` here would be worse than a stale levelHeader_: it would route the
    // NEXT level's save through the wrong writer entirely, not just lose a field it carries.
    levelIsLegacyOcmap_ = false;
    legacyMapHeader_ = fmt::OcMapData{};
    entityLegacyDeform_.clear();
    entityLegacySurface_.clear();
    entityLegacyMaterial_.clear();
    undoStack_.clear();
    redoStack_.clear();
    // A cleared history means nothing is pending against the file either.
    markLevelSaved();
    editToEntity_.clear();
    entityToEdit_.clear();
    editBeforeValid_ = false;
    if (sel_ == kSelScene) { sel_ = -1; selEntity_ = scene::kInvalidEntity; }
#if AVER_MODULE_PHYSICS
    for (const int32_t b : levelBodies_) aver_phys_remove_body(b);
    levelBodies_.clear();
#endif
#if AVER_FLUIDS_SIMULATED
    // THE ONLY teardown site for a simulated volume, belonging here rather than in
    // applyLevelWater: this runs on every path that ends a level (including File > New Level,
    // which never calls applyLevelWater), so without it a simulated pool outlived its level.
    // NOT folded into the levelBodies_ loop: the volume's body is never pushed there, and removing
    // it directly would double-free once FluidScene::retire removes it too.
    if (fluidHandle_) {
        fluidSurfaceMaterial_.erase(fluidHandle_);
        fluidScene_.despawn(fluidHandle_);
        fluidHandle_ = 0;
    }
    // Cleared for the "simulated level -> level with no water" case: applyLevelWater returns
    // immediately when the new level declares no WATER record, so a latch left standing here
    // would spawn the OLD level's volume into the new world on the very next frame.
    fluidWantPending_ = false;
    // Same reasoning, generalised to every graph-authored volume this level's actors spawned:
    // none are pushed to levelBodies_ either, and a class outliving its own level's teardown would
    // go on sloshing in the next one, the same way an un-despawned fluidHandle_ used to.
    for (const fluids::FluidHandle h : fluidGraphHandles_) {
        fluidSurfaceMaterial_.erase(h);
        fluidScene_.despawn(h);
    }
    fluidGraphHandles_.clear();
    fluidGraphQueue_.clear();
#endif
    hasLevelFog_ = false;
    hasLevelSun_ = false;
    hasLevelSky_ = false;
    hasLevelClouds_ = false;
    // BACK TO THE EDITOR DEFAULT, now that the slider IS the saved value. While fog lived in two
    // members this happened for free: the level's copy went out of scope with levelFog_ and the
    // slider had never been touched. With one member, leaving it alone would carry one level's
    // weather into the next one that declares none.
    fogDensity_ = 4e-6f;
    levelPath_.clear();
#if AVER_MODULE_LANDSCAPE
    unloadLandscape(eng.device());
#endif
}

#endif

} // namespace aver
