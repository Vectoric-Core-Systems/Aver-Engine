// GameLandscape: the runtime's own copy of the editor's landscape subsystem -- one resident
// .ocland section (render + collision) plus a small ring of procedural tiles kept resident around
// the camera. Ported from SandboxApp's landscape members and functions
// (sandbox/src/SandboxLevelLoad.cpp) -- see each method's own comment for which one it mirrors.
//
// NOT A GOD OBJECT: takes a device, a content directory and the level's world data as arguments
// rather than reaching into GameApp for them. GameApp owns one of these by value, the same way it
// owns GameContent and GameLevel.
//
// LEFT OUT, EDITOR-ONLY:
//   - sculpting (sandbox/src/SandboxApp.hpp's sculptRadiusCm_ and friends, HeightfieldRay.hpp's
//     raycastHeightfield, Sculpt.hpp) -- a shipped game has no brush.
//   - the --landscape command-line override (landscapeCliOverride_) -- a level's own LANDSCAPE
//     record, or the levelname.ocland convention, is the only way a game resolves its terrain.
//   - landscapeDirty_ and every save path -- a shipped game never writes its level back out.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcWorld.hpp"

#if AVER_MODULE_LANDSCAPE
#include "aver/formats/OcLand.hpp"
#include "aver/landscape/LandscapeTree.hpp"
#include "aver/landscape/LandscapeRenderer.hpp"
#include "aver/landscape/TerrainTile.hpp"
#include "aver/landscape/TerrainNoise.hpp"

#if AVER_MODULE_PBR
#include "aver/pbr/MaterialSystem.hpp"
#endif

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

// Forward-declared UNCONDITIONALLY (regardless of AVER_MODULE_PBR), so loadForLevel/updateRingTiles
// below can take a pbr::MaterialSystem* parameter even in a build with the PBR module absent -- there
// it is always null and never dereferenced (every real use is inside an `#if AVER_MODULE_PBR` block
// in GameLandscape.cpp). The full type is pulled in above, under its own AVER_MODULE_PBR guard, for
// the translation units that do dereference it; a forward declaration ahead of that later, complete
// definition is ordinary and conflict-free.
namespace aver::pbr { class MaterialSystem; }

namespace aver::game {

class GameContent;

class GameLandscape {
public:
    // ---- one resident section, per level ----

    // Resolves and loads this level's terrain, mirroring SandboxApp::loadLandscapeForLevel:
    //   1. the level's own LANDSCAPE record in `w`, its section resolved against `contentDir` and
    //      placed at the record's own `at` (overriding the .ocland file's own originCm);
    //   2. failing that (or with no record at all), the <levelname>.ocland convention beside
    //      `levelPath`.
    // Silent when neither resolves -- most levels have no terrain. `device` may be null (a section
    // still loads with no device; forgetAll/draw simply have nothing to free/create yet).
    //
    // Also rebuilds the collision body (rebuildCollision) and, when `content` and `materials` are
    // both non-null, binds the LANDSCAPE material right away (applySurfaceToAll) -- the tail of the
    // editor's own loadLandscapeForLevel. Pass null for either when the caller's material system is
    // not up yet (mirrors SandboxRender.cpp's own "level load cannot be trusted to be late enough"
    // comment) and call applySurfaceToAll(...) again once it is.
    void loadForLevel(rhi::IDevice* device, const std::string& contentDir, const std::string& levelPath,
                       const fmt::OcWorldData& w, GameContent* content = nullptr,
                       pbr::MaterialSystem* materials = nullptr);

    // Frees the resident section's meshes (when `device` is non-null), its collision body, and every
    // ring tile. Mirrors SandboxApp::unloadLandscape. Safe to call on an already-unloaded instance.
    void unload(rhi::IDevice* device);

    bool loaded() const { return loaded_; }
    const std::string& path() const { return landscapePath_; }

    // The ground height at world (x, y) -- the SAME query SandboxApp::loadLevel's own
    // opt.groundHeightAt lambda and SandboxApp::spawnClassPlacements' snap branch use: a straight
    // bilinear lookup into the resident section (landscape::surfaceHeightAt), false outside its
    // footprint or with nothing loaded. Wire this into world::InstantiateOptions::groundHeightAt and
    // into a `snap` class placement's own z resolution (`z = groundHeightAt(x,y) + authored z`).
    bool groundHeightAt(f64 worldXCm, f64 worldYCm, f64& outWorldZCm) const;

    // The height chunk-streamed scatter follows: the resident section where it exists, and past its
    // rim the same continuous noise the ring tiles use -- the editor's heightSource lambda in
    // setChunkStreamingEnabled. Always answers; only meaningful while loaded().
    bool scatterHeightAt(f32 worldXCm, f32 worldYCm, f32& outWorldZCm) const;

    // ---- the ring: procedural tiles past the authored section's own rim ----

    // Keeps a small window of procedural tiles resident around (cameraXCm, cameraYCm), so the
    // terrain extends past the authored section's own rim. Cheap to call every frame -- real work
    // only happens the frame the camera's own tile coordinate changes. Mirrors
    // SandboxApp::updateLandscapeRingTiles. A no-op when no section is loaded. `content`/`materials`
    // (both required together) bind the LANDSCAPE material onto any newly-synthesized tile, the same
    // as the editor's own call -- a tile born mid-session otherwise renders with the renderer's flat
    // fallback colour until the next call supplies both.
    void updateRingTiles(rhi::IDevice* device, f32 cameraXCm, f32 cameraYCm, GameContent* content = nullptr,
                          pbr::MaterialSystem* materials = nullptr);

    // Draws the resident section and every ring tile: the editor's landscape pass in
    // SandboxApp::onRender (SandboxRender.cpp) -- LOD select, then draw with an identity transform,
    // each tile taking one share of the draw budget. Retries the surface binding first until it
    // holds, as the editor does, when `content` and `materials` are given. `viewportHeightPx` sets the
    // LOD projection scale for the 60-degree vertical field of view both hosts render with.
    void draw(rhi::IDevice& device, const Vec3& eye, const Mat4& viewProj, f32 viewportHeightPx,
              GameContent* content = nullptr, pbr::MaterialSystem* materials = nullptr);

    // Called at the end of loadForLevel, where the editor restarts live chunk streaming
    // (applyLandscapeToStreaming) so scatter regenerates against the new surface.
    void setTerrainChangedHook(std::function<void()> hook) { terrainChanged_ = std::move(hook); }

    // One ring tile's data, quadtree and renderer. Same three fields as SandboxApp's own
    // LandscapeRingTile (sandbox/src/SandboxApp.hpp).
    struct RingTile {
        fmt::OcLandData data;
        landscape::LandscapeTree tree;
        std::unique_ptr<landscape::LandscapeRenderer> renderer;
    };

#if AVER_MODULE_PBR
    // Re-applies the LANDSCAPE material to the authored section and every resident ring tile at
    // once, and evicts every cached mesh if the resolved UV tiling changed under them. Mirrors
    // SandboxApp::applyLandscapeSurfaceToAll. `materials` is the host's pbr::MaterialSystem (GameApp's
    // voxiRenderer_.materials()) -- this class never constructs or owns one, matching GameContent's
    // own "take the material lookup as a hook" shape (Runtime/include/aver/game/GameContent.hpp).
    //
    // loadForLevel and draw call this themselves when given `content`/`materials`.
    void applySurfaceToAll(rhi::IDevice* device, GameContent& content, pbr::MaterialSystem& materials);
#endif

private:
    // Loads one .ocland section and builds its quadtree over it. Pure CPU. Calls unload() first, so
    // `device` frees whatever section is currently resident before this one replaces it (null is only
    // safe when nothing is resident yet). Mirrors SandboxApp::loadLandscape.
    bool loadLandscape(rhi::IDevice* device, const std::string& path);

    // Rebuilds the resident section's static collision body from scratch (Jolt's heightfield shape
    // is immutable once created -- there is no partial update). Mirrors
    // SandboxApp::rebuildLandscapeCollision. A no-op without AVER_MODULE_PHYSICS.
    void rebuildCollision();

    std::function<void()> terrainChanged_;   // see setTerrainChangedHook

#if AVER_MODULE_PBR
    // Pushes the level's LANDSCAPE material into one renderer, as an opaque binding, and resolves the
    // UV tiling from it. Mirrors SandboxApp::applyLandscapeSurface -- see that function's own comment
    // for why the host (not this module) does the resolving, and why the tiling has to be re-asked for
    // with pbr::MaterialLibrary::get().valid(h) rather than trusted on the first constants() call.
    void applySurface(landscape::LandscapeRenderer& r, GameContent& content, pbr::MaterialSystem& materials);
#endif

    // The material name the level's LANDSCAPE record named, held as TEXT rather than a resolved
    // handle -- see SandboxApp::landscapeMaterial_'s own comment for why (the material system isn't
    // necessarily ready when the level loads).
    std::string landscapeMaterial_;
    // World centimetres per texture tile for the landscape mesh's baked UVs. Defaulted to the same
    // 1000 LandscapeRenderer::draw() itself defaults to.
    f32 landscapeUvTilingCm_ = 1000.0f;
    // False until the tiling above came from a material this class actually considers valid, not
    // MaterialSystem's fallback. See SandboxApp::landscapeUvTilingResolved_'s own comment.
    bool landscapeUvTilingResolved_ = false;
    bool loaded_ = false;
    std::string landscapePath_;   // the section actually resident; empty when none is
    // The static body the terrain collides through, or -1. Unconditional (not AVER_MODULE_PHYSICS-
    // guarded) for the same reason SandboxApp::landscapeBody_ is: guarded at every USE instead, so the
    // declaration cannot go out of scope from under a call site guarded differently.
    i32 landscapeBody_ = -1;

    fmt::OcLandData landscapeData_;
    landscape::LandscapeTree landscapeTree_;
    // Heap-owned so unload() can destroy and recreate it independently of the section data, mirroring
    // LandscapeRenderer's own forget-then-discard lifecycle.
    std::unique_ptr<landscape::LandscapeRenderer> landscapeRenderer_;

    std::unordered_map<landscape::TileCoord, RingTile> landscapeRingTiles_;
    landscape::TerrainNoiseParams landscapeNoiseParams_;   // shared by every ring tile and groundHeightAt's own section
    static constexpr i32 kRingRadius = 1;                  // tiles each side of the camera's tile: 3x3
    static constexpr u32 kMaxSectionsResident =
        (2 * kRingRadius + 1) * (2 * kRingRadius + 1);   // 9
    // A SHARE of the renderer's one shared draw/resident-node budget, not the whole thing -- see
    // SandboxApp::kLandscapeMaxDrawsPerTile's own comment (docs/LANDSCAPE_EDITOR.md blocker 9).
    static constexpr u32 kMaxDrawsPerTile = 192u / kMaxSectionsResident;
    static constexpr u32 kMaxResidentNodesPerTile = 512u / kMaxSectionsResident;
    landscape::TileCoord lastCameraTile_{};
    bool lastCameraTileValid_ = false;
};

} // namespace aver::game

#endif // AVER_MODULE_LANDSCAPE
