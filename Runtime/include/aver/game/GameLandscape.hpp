// GameLandscape: one resident .ocland section (render + collision) plus a small ring of
// procedural tiles kept resident around the camera, plus the authoring operations an in-process
// level editor needs to sculpt, generate and save one. Both the editor and a shipped game host
// this same class.
//
// NOT A GOD OBJECT: takes a device, a content directory and the level's world data as arguments
// rather than reaching into GameApp for them. GameApp owns one of these by value, the same way it
// owns GameContent and GameLevel.
//
// AUTHORING IS EDITOR-ONLY: a shipped game calls none of setPathOverride() through save() below --
// it resolves its terrain from the level's own LANDSCAPE record or the levelname.ocland convention
// (loadForLevel) and never edits or writes it back out. The brush ring itself (radius, strength,
// which tool is active, raycasting the cursor into the section) and the Landscape panel's UI stay
// editor-side; this class only holds the section's heights and the stroke/undo bookkeeping a host
// needs to apply one brush tick and capture a whole drag as one undoable edit.
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
#include "aver/landscape/Sculpt.hpp"

#if AVER_MODULE_PBR
#include "aver/pbr/MaterialSystem.hpp"
#endif

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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

    // Resolves and loads this level's terrain:
    //   0. setPathOverride()'s path, if non-empty -- used as-is, with the LANDSCAPE record below
    //      not read at all (its material and its `at` placement are both skipped). Editor-only.
    //   1. failing that, the level's own LANDSCAPE record in `w`, its section resolved against
    //      `contentDir` and placed at the record's own `at` (overriding the .ocland file's own
    //      originCm);
    //   2. failing that (or with no record at all), the <levelname>.ocland convention beside
    //      `levelPath`.
    // Silent when none resolves -- most levels have no terrain. `device` may be null (a section
    // still loads with no device; forgetAll/draw simply have nothing to free/create yet).
    //
    // Also rebuilds the collision body (rebuildCollision) and, when `content` and `materials` are
    // both non-null, binds the LANDSCAPE material right away (applySurfaceToAll). Pass null for
    // either when the caller's material system is not up yet -- level load cannot be trusted to be
    // late enough for that, since MaterialSystem::ready() also needs its own GPU side up, with no
    // guaranteed order against the level load -- and call applySurfaceToAll(...) again once it is.
    void loadForLevel(rhi::IDevice* device, const std::string& contentDir, const std::string& levelPath,
                       const fmt::OcWorldData& w, GameContent* content = nullptr,
                       pbr::MaterialSystem* materials = nullptr);

    // Frees the resident section's meshes (when `device` is non-null), its collision body, and every
    // ring tile, plus every bit of authoring state: dirty(), a stroke in progress, and any pending
    // GPU invalidate. Safe to call on an already-unloaded instance.
    void unload(rhi::IDevice* device);

    bool loaded() const { return loaded_; }
    const std::string& path() const { return landscapePath_; }

    // The ground height at world (x, y): a straight bilinear lookup into the resident section
    // (landscape::surfaceHeightAt), false outside its footprint or with nothing loaded. Wired into
    // world::InstantiateOptions::groundHeightAt and into a `snap` class placement's own z resolution
    // (`z = groundHeightAt(x,y) + authored z`).
    bool groundHeightAt(f64 worldXCm, f64 worldYCm, f64& outWorldZCm) const;

    // The height chunk-streamed scatter follows: the resident section where it exists, and past its
    // rim the same continuous noise the ring tiles use. Always answers; only meaningful while
    // loaded().
    bool scatterHeightAt(f32 worldXCm, f32 worldYCm, f32& outWorldZCm) const;

    // ---- the ring: procedural tiles past the authored section's own rim ----

    // Keeps a small window of procedural tiles resident around (cameraXCm, cameraYCm), so the
    // terrain extends past the authored section's own rim. Cheap to call every frame -- real work
    // only happens the frame the camera's own tile coordinate changes. A no-op when no section is
    // loaded. `content`/`materials` (both required together) bind the LANDSCAPE material onto any
    // newly-synthesized tile -- a tile born mid-session otherwise renders with the renderer's flat
    // fallback colour until the next call supplies both.
    void updateRingTiles(rhi::IDevice* device, f32 cameraXCm, f32 cameraYCm, GameContent* content = nullptr,
                          pbr::MaterialSystem* materials = nullptr);

    // Draws the resident section and every ring tile: LOD select, then draw with an identity
    // transform, each tile taking one share of the draw budget. Retries the surface binding first
    // until it holds, when `content` and `materials` are given. `viewportHeightPx` sets the LOD
    // projection scale for the 60-degree vertical field of view both hosts render with.
    void draw(rhi::IDevice& device, const Vec3& eye, const Mat4& viewProj, f32 viewportHeightPx,
              GameContent* content = nullptr, pbr::MaterialSystem* materials = nullptr);

    // Called at the end of loadForLevel, where the host restarts live chunk streaming so scatter
    // regenerates against the new surface.
    void setTerrainChangedHook(std::function<void()> hook) { terrainChanged_ = std::move(hook); }

    // ---- authoring: editor-only, a shipped game calls none of this ----

    // A section path loadForLevel uses INSTEAD of the level's LANDSCAPE record and the
    // <level>.ocland convention. When non-empty the record is not read at all -- its material and
    // its `at` placement are both ignored. Empty clears it.
    void setPathOverride(std::string path) { pathOverride_ = std::move(path); }

    // Public form of the private loadLandscape: unload, load the .ocland, build its tree, widen
    // skirts, reset hysteresis, a new renderer, loaded()/path() set, dirty() cleared. No collision
    // rebuild, no surface binding, no terrain-changed hook -- a caller that needs those calls them
    // itself afterward.
    bool loadSection(rhi::IDevice* device, const std::string& path) { return loadLandscape(device, path); }

    // The resident section's own data: heights, sampleCount, spacingCm, originCm, bounds. Empty
    // (sampleCount 0) when nothing is loaded.
    const fmt::OcLandData& data() const { return landscapeData_; }

    // The ring/noise parameters an authoring UI edits in place -- also what the ring tiles and
    // scatterHeightAt's past-the-rim fallback read.
    landscape::TerrainNoiseParams& noiseParams() { return landscapeNoiseParams_; }
    const landscape::TerrainNoiseParams& noiseParams() const { return landscapeNoiseParams_; }

    // True once heights or origin have been edited since the last load or save.
    bool dirty() const { return dirty_; }

    // Rebuilds ONLY if the heights have moved since the last build, and does nothing otherwise.
    //
    // WHY THIS EXISTS. The rule above is right about FREQUENCY and was wrong about one moment. Its
    // two call sites mean a sculpted, undone or freshly generated section keeps collision from
    // BEFORE the edit until somebody clicks Save -- and nothing forced a save on the way into Play.
    // So the most ordinary sequence a user performs to check their work -- sculpt a hill, press Play,
    // walk onto it -- put them through the old surface, which is indistinguishable from "terrain
    // editing does not work".
    //
    // ENTERING PLAY IS EXACTLY WHEN COLLISION MUST BE CURRENT, and it is once, not per tick: the
    // frequency argument above is untouched. Editing still never rebuilds, because nothing walks on
    // terrain while you are sculpting it.
    //
    // Guarded by its own flag rather than dirty(), which means "there are unsaved changes" and is
    // cleared by saving -- a save that rebuilt collision and a save that did not would otherwise be
    // indistinguishable here.
    void rebuildCollisionIfStale();

    // Writes data().originCm and marks dirty. NO tree/ring rebuild -- an authoring UI that just
    // wants to record where a section sits leaves the tree, which caches centres and bounds derived
    // from originCm, to whatever rebuilds it next (loadForLevel's own placement branch does this
    // properly, with a rebuild; this does not).
    void setOriginCm(f32 x, f32 y, f32 z);

    // Writes one stored rect of samples back into the section -- shared by undo and redo, which
    // differ only in which of the two stored buffers they pass as `src`. Returns false and leaves
    // the section untouched when nothing is loaded, `src` is empty, or the rect is out of range (the
    // section was reloaded at another size since the rect was captured). On success: recomputes
    // boundsMin/boundsMax[2] (writing samples directly bypasses applyBrush's own bounds update),
    // rebuilds the tree (false, logged, if that fails), widens skirts, resets hysteresis, latches a
    // pending GPU invalidate of the rect (see flushPendingInvalidate), marks dirty, and returns true.
    bool applyHeightRect(u32 x0, u32 y0, u32 x1, u32 y1, const std::vector<f32>& src);

    // Drains the invalidate applyHeightRect latches: when pending, clears it and, with both a
    // renderer and a device, forgets the GPU meshes overlapping the latched rect. The GPU half is
    // deferred a frame on purpose -- applyHeightRect (reached from undo/redo) runs in places with no
    // device to hand, while the heightfield and quadtree it edits update synchronously so queries see
    // the change immediately; only the cached GPU meshes lag by a frame, invisibly.
    void flushPendingInvalidate(rhi::IDevice* device);

    // Starts a sculpt stroke: nothing touched yet. The pre-stroke heights are captured lazily as the
    // rect grows (see growStroke), not up front -- at stroke start the rect a drag will end up
    // covering is not known yet.
    void beginStroke();

    // Unions this tick's touched rect into the stroke's, capturing the pre-stroke heights of anything
    // newly covered. Must run BEFORE the tick's own height edit, or the "before" it captures is
    // already the "after". A no-op with an empty rect or no stroke active.
    void growStroke(const landscape::BrushRect& r);

    bool strokeActive() const { return strokeActive_; }

    // Ends a stroke and reports what changed. Returns false (and clears the captured buffer) when no
    // stroke was active, when nothing was ever touched, or when nothing is loaded. Otherwise fills
    // `after` from the section over the touched rect, moves the captured pre-stroke heights into
    // `before`, sets [x0,y0]-[x1,y1] to the touched rect, and returns true -- unless `before` and
    // `after` come out equal (a stroke that changed nothing, e.g. clicking on terrain already at the
    // flatten target), in which case the buffer is cleared and this returns false too. The caller
    // owns turning a true return into an undo entry.
    bool endStroke(std::vector<f32>& before, std::vector<f32>& after, u32& x0, u32& y0, u32& x1, u32& y1);

    // One sculpt tick: grows the active stroke over the brush's footprint, applies it at `amount`
    // (0..1, how much of the mode's per-tick strength to spend this tick), and -- if anything was
    // touched -- rebuilds the tree, widens skirts, resets hysteresis, and (with both a renderer and a
    // device) forgets the touched GPU meshes immediately, unlike applyHeightRect: a live brush has a
    // device on hand every tick, so there is nothing to defer. Marks dirty whether or not the rebuild
    // succeeded. Returns the touched rect (empty if the brush touched no sample, in which case
    // nothing else above runs).
    landscape::BrushRect sculpt(rhi::IDevice* device, const landscape::BrushParams& p, f32 amount);

    // Writes the resident section back to path(). Returns false when nothing is loaded, path() is
    // empty, or the write fails (with `why` filled, when given) -- logging either way is the caller's
    // job. On success clears dirty() and rebuilds collision: saving is the one deliberate commit
    // point for a sculpt's collision snapshot (never rebuilt per stroke, per undo, or on create --
    // see rebuildCollision's own comment).
    bool save(std::string* why = nullptr);

    // One ring tile's data, quadtree and renderer.
    struct RingTile {
        fmt::OcLandData data;
        landscape::LandscapeTree tree;
        std::unique_ptr<landscape::LandscapeRenderer> renderer;
    };

#if AVER_MODULE_PBR
    // Re-applies the LANDSCAPE material to the authored section and every resident ring tile at
    // once, and evicts every cached mesh if the resolved UV tiling changed under them. `materials` is
    // the host's pbr::MaterialSystem (GameApp's voxiRenderer_.materials()) -- this class never
    // constructs or owns one, matching GameContent's own "take the material lookup as a hook" shape
    // (Runtime/include/aver/game/GameContent.hpp).
    //
    // loadForLevel and draw call this themselves when given `content`/`materials`.
    void applySurfaceToAll(rhi::IDevice* device, GameContent& content, pbr::MaterialSystem& materials);
#endif

private:
    // Loads one .ocland section and builds its quadtree over it. Pure CPU. Calls unload() first, so
    // `device` frees whatever section is currently resident before this one replaces it (null is only
    // safe when nothing is resident yet). Clears dirty_ -- a freshly loaded section has nothing
    // pending to save.
    bool loadLandscape(rhi::IDevice* device, const std::string& path);

    // Rebuilds the resident section's static collision body from scratch (Jolt's heightfield shape
    // is immutable once created -- there is no partial update). Called from loadForLevel and save()
    // only -- NEVER per sculpt tick, per undo/redo, or on generating fresh noise, which would rebuild
    // it far more often than any of those actually needs collision to be current. A no-op without
    // AVER_MODULE_PHYSICS.
    void rebuildCollision();


    std::function<void()> terrainChanged_;   // see setTerrainChangedHook

#if AVER_MODULE_PBR
    // Pushes the level's LANDSCAPE material into one renderer, as an opaque binding, and resolves the
    // UV tiling from it. The host (not this module) does the resolving because the material system's
    // own readiness and the level load race each other with no guaranteed order; the tiling has to be
    // re-asked for with pbr::MaterialLibrary::get().valid(h) rather than trusted on the first
    // constants() call, since constants() itself returns a fallback for any handle not yet valid.
    void applySurface(landscape::LandscapeRenderer& r, GameContent& content, pbr::MaterialSystem& materials);
#endif

    // The material name the level's LANDSCAPE record named, held as TEXT rather than a resolved
    // handle: the material system isn't necessarily ready when the level loads, and re-resolving from
    // the name lets applySurfaceToAll() run again without caring which finished first.
    std::string landscapeMaterial_;
    // World centimetres per texture tile for the landscape mesh's baked UVs. Defaulted to the same
    // 1000 LandscapeRenderer::draw() itself defaults to.
    f32 landscapeUvTilingCm_ = 1000.0f;
    // False until the tiling above came from a material this class actually considers valid, not
    // MaterialSystem's fallback. Separate from LandscapeRenderer::hasSurfaceBinding(): that latches on
    // the FIRST successful apply, which can predate the material being resolved, freezing the tiling
    // at the fallback's 200cm forever.
    bool landscapeUvTilingResolved_ = false;
    bool loaded_ = false;
    std::string landscapePath_;   // the section actually resident; empty when none is
    // The static body the terrain collides through, or -1. Unconditional (not AVER_MODULE_PHYSICS-
    // guarded) so the declaration cannot go out of scope from under a call site guarded differently --
    // rebuildCollision/unload guard every USE instead.
    i32 landscapeBody_ = -1;

    // ---- authoring state ----
    std::string pathOverride_;   // see setPathOverride
    bool dirty_ = false;         // see dirty()
    // Heights have moved since the collision body was last built. SEPARATE FROM dirty_ on purpose:
    // dirty_ answers "does the user have unsaved work" and is cleared by saving, while this answers
    // "does the physics shape still describe the mesh" and is cleared by rebuilding. Saving happens
    // to do both today, which is exactly why one flag could not serve -- a future save that skipped
    // the rebuild would silently take collision with it. See rebuildCollisionIfStale().
    bool collisionStale_ = false;

    // Latched by applyHeightRect, drained by flushPendingInvalidate -- see each one's own comment.
    bool pendingInvalidate_ = false;
    u32  pendingX0_ = 0, pendingY0_ = 0, pendingX1_ = 0, pendingY1_ = 0;

    // The in-progress sculpt stroke -- see beginStroke/growStroke/endStroke.
    bool strokeActive_ = false, strokeEmpty_ = true;
    u32  strokeX0_ = 0, strokeY0_ = 0, strokeX1_ = 0, strokeY1_ = 0;
    std::vector<f32> strokeBefore_;

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
    // A SHARE of the renderer's one shared draw/resident-node budget, not the whole thing: the
    // renderer's transient constant ring is a fixed, shared allotment no matter how many sections are
    // resident, so the single section's own long-safe defaults (192 draws, 512 cached meshes) are a
    // TOTAL split evenly across the largest window this ring can hold (docs/LANDSCAPE_EDITOR.md
    // blocker 9).
    static constexpr u32 kMaxDrawsPerTile = 192u / kMaxSectionsResident;
    static constexpr u32 kMaxResidentNodesPerTile = 512u / kMaxSectionsResident;
    landscape::TileCoord lastCameraTile_{};
    bool lastCameraTileValid_ = false;
};

} // namespace aver::game

#endif // AVER_MODULE_LANDSCAPE
