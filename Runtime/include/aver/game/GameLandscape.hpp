// GameLandscape: the level's resident .ocland sections (render + collision) -- a primary section plus any
// further LANDSCAPE records, each placed at its own `at` -- plus a small ring of procedural tiles kept resident
// around the camera past their rim, plus the authoring operations an in-process level editor needs to sculpt,
// generate and save the primary one. Both the editor and a shipped game host this same class.
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
#include "aver/rhi/WorldShift.hpp"
#include "aver/landscape/LandscapeTree.hpp"
#include "aver/landscape/LandscapeRenderer.hpp"
#include "aver/landscape/TerrainTile.hpp"
#include "aver/landscape/TerrainNoise.hpp"
#include "aver/landscape/Sculpt.hpp"
#include "aver/landscape/Paint.hpp"
#include "aver/landscape/Carve.hpp"
#include "aver/landscape/HeightfieldRay.hpp"

#if AVER_MODULE_PBR
#include "aver/pbr/MaterialSystem.hpp"
#endif

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
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
    // ---- the resident sections, per level ----

    // Resolves and loads this level's terrain:
    //   0. setPathOverride()'s path, if non-empty -- used as-is, with the LANDSCAPE record below
    //      not read at all (its material and its `at` placement are both skipped). Editor-only.
    //   1. failing that, the level's own LANDSCAPE record in `w`, its section resolved against
    //      `contentDir` and placed at the record's own `at` (overriding the .ocland file's own
    //      originCm);
    //   2. failing that (or with no record at all), the <levelname>.ocland convention beside
    //      `levelPath`.
    // The first resolved section is the PRIMARY one (data(), path(); section index 0). With branch 1, every further LANDSCAPE
    // record loads too as an extra section (index 1..) at its own `at`: rendered, collided, queried and AUTHORED like the primary
    // (the brush, paint, carve and save act on every section they reach, see "sections" below), sharing the level's terrain
    // material (each extra has its own paint weights); a record whose file is missing is skipped with a warning.
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

    // Frees every resident section's meshes (when `device` is non-null), its collision body, and every
    // ring tile, plus every bit of authoring state: dirty(), a stroke in progress, and any pending
    // GPU invalidate. Safe to call on an already-unloaded instance.
    void unload(rhi::IDevice* device);

    bool loaded() const { return loaded_; }
    const std::string& path() const { return landscapePath_; }

    // The ground height at world (x, y): a straight bilinear lookup into whichever resident section contains the
    // point (landscape::surfaceHeightAt), false outside every footprint or with nothing loaded. Wired into
    // world::InstantiateOptions::groundHeightAt and into a `snap` class placement's own z resolution
    // (`z = groundHeightAt(x,y) + authored z`).
    bool groundHeightAt(f64 worldXCm, f64 worldYCm, f64& outWorldZCm) const;

    // The height chunk-streamed scatter follows: the resident sections where they exist, and past their
    // rim the same continuous noise the ring tiles use. Always answers; only meaningful while
    // loaded().
    bool scatterHeightAt(f32 worldXCm, f32 worldYCm, f32& outWorldZCm) const;

    // ---- the ring: procedural tiles past the authored sections' rim ----

    // Keeps a small window of procedural tiles resident around (cameraXCm, cameraYCm), so the
    // terrain extends past the authored sections' rim (the grid follows the primary section; a tile mostly covered by
    // an authored section is not built). Cheap to call every frame -- real work
    // only happens the frame the camera's own tile coordinate changes. A no-op when no section is
    // loaded. `content`/`materials` (both required together) bind the LANDSCAPE material onto any
    // newly-synthesized tile -- a tile born mid-session otherwise renders with the renderer's flat
    // fallback colour until the next call supplies both.
    void updateRingTiles(rhi::IDevice* device, f32 cameraXCm, f32 cameraYCm, GameContent* content = nullptr,
                          pbr::MaterialSystem* materials = nullptr);

    // Draws the resident sections and ring tiles: LOD select, then draw with an identity transform. Only the nearest
    // kMaxDrawnSections of those inside the frustum and kDrawRadiusCm draw; they share one draw budget, nearest first,
    // and a section out of view for a while drops its GPU meshes. Retries the surface binding first
    // until it holds, when `content` and `materials` are given. `viewportHeightPx` sets the LOD
    // projection scale from `fovYRad`, the vertical field of view the host built the camera with (60 degrees unless a
    // gameplay camera rig changed it: pass PlayView::fovYRad).
    void draw(rhi::IDevice& device, const Vec3& eye, const Mat4& viewProj, f32 viewportHeightPx,
              GameContent* content = nullptr, pbr::MaterialSystem* materials = nullptr, f32 fovYRad = radians(60.0f));

    // ---- the floating origin (LARGE_WORLD 2.2.6) ----
    // Everything this class holds and answers in the scene frame is origin-relative, like the scene: the section's and
    // every ring tile's originCm, the quadtree, the physics heightfield (a static body the physics shift moves), the
    // picking/sculpt/paint and surface queries. The sidecar on disk, a LANDSCAPE record's `at`, the noise past the rim
    // and groundHeightAt's arguments are ABSOLUTE; the class converts at exactly those seams.
    //
    // The origin moved: the data and the tree follow by the delta (a plain f32 subtraction), and the renderers are told the
    // new frame, so a resident node is drawn translated by (its build frame - the new one) instead of being rebuilt. Needs
    // the terrain parameter texture re-uploaded when the section is painted (it carries the section's origin). Idempotent:
    // it moves to info.newOrigin, so applying one event twice, or after setFrameOrigin, changes nothing.
    void onOriginShift(const rhi::ShiftWorldInfo& info) { moveFrameTo(info.newOrigin); }
    // Names the frame without an event: a host that resets its origin at level load (WorldOrigin::reset runs no
    // participant) calls it BEFORE loadForLevel, so the section is loaded into that frame. Same as an event otherwise.
    void setFrameOrigin(const rhi::ShiftOrigin& o) { moveFrameTo(o); }
    const rhi::ShiftOrigin& frameOrigin() const { return frame_; }
    // The primary section's sample (0,0) in ABSOLUTE cm, exact (data().originCm is the scene-frame f32). What a LANDSCAPE record's
    // `at` and anything else that is written to a level or a save must carry; equals data().originCm at origin zero.
    void homeOriginAbsCm(f64 out[3]) const { out[0] = homeAbs_[0]; out[1] = homeAbs_[1]; out[2] = landscapeData_.originCm[2]; }

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

    // The primary section's own data: heights, sampleCount, spacingCm, originCm, bounds. Empty
    // (sampleCount 0) when nothing is loaded.
    const fmt::OcLandData& data() const { return landscapeData_; }

    // The extra sections (further LANDSCAPE records) and the resident ring tiles, for hosts and tests.
    u32 extraSectionCount() const { return static_cast<u32>(extras_.size()); }
    const fmt::OcLandData& extraData(u32 i) const { return extras_[i].data; }
    // ---- sections: the primary (index 0) and the extras (1..) as one list ----
    // Authoring acts on whichever sections it reaches. A brush, carve or fill edits every section whose footprint it overlaps;
    // where two sections share edge samples (same world position) the stroke leaves them equal. One drag is one undo entry
    // that lists every section it touched (HeightEdit / WeightEdit, one per section). save() writes every dirty section to its
    // own file; a LANDSCAPE record's `at` is never touched by any of it.
    u32 sectionCount() const { return loaded_ ? 1u + static_cast<u32>(extras_.size()) : 0u; }
    const fmt::OcLandData& sectionData(u32 i) const { return i == 0 ? landscapeData_ : extras_[i - 1].data; }
    const std::string& sectionPath(u32 i) const { return i == 0 ? landscapePath_ : extras_[i - 1].path; }
    bool sectionDirty(u32 i) const;
    // The nearest hit of a ray (scene frame) among all sections; `section` (optional) receives which one.
    bool raycast(const f32 ro[3], const f32 rd[3], landscape::HeightfieldHit& out, u32* section = nullptr,
                 f32 maxDistCm = 200000.0f) const;
    // The section holding scene-frame (x, y), or null. surfaceHeightAtScene is the same lookup answering the height.
    const fmt::OcLandData* sectionAt(f32 xCm, f32 yCm) const;
    bool surfaceHeightAtScene(f32 xCm, f32 yCm, f32& outZ) const;
    // The painted weight (0..1) of the layer called `name` at scene-frame (x, y); 0 for an unknown layer or no section there.
    // What a biome's layer mask and layer gates read when a streamed world generates on this landscape.
    f32 layerWeightAtScene(std::string_view name, f32 xCm, f32 yCm) const;

    // What one section's share of a stroke changed: a rect of samples before / after (undo / redo write them back).
    struct HeightEdit {
        u32 section = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        std::vector<f32> before, after;
    };
    // The same for paint: `planes` stored weight planes over the rect, plane-major.
    struct WeightEdit {
        u32 section = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0, planes = 0;
        std::vector<u8> before, after;
    };
    // Ends the stroke and lists every section that changed (empty = nothing to undo, false). One undo entry carries the list.
    bool endStroke(std::vector<HeightEdit>& out);
    bool endWeightStroke(std::vector<WeightEdit>& out);
    // Undo / redo of one section's rect; same contract as the primary-only forms below.
    bool applyHeightRect(u32 section, u32 x0, u32 y0, u32 x1, u32 y1, const std::vector<f32>& src);
    bool applyWeightRect(u32 section, u32 x0, u32 y0, u32 x1, u32 y1, u32 planes, const std::vector<u8>& src);
    // Paint the material layer `layerName` on every section the brush reaches (p.layer is ignored): each such section gets the
    // layer added on the spot (before its share of the stroke is captured). Returns the first touched section's rect.
    landscape::BrushRect paint(rhi::IDevice* device, const landscape::PaintParams& p, f32 amount, const std::string& layerName);
    // Whole-terrain edits over every section (inside a weight stroke they are one undo entry).
    void fillLayerAll(const std::string& layerName);
    void autoFillBySlopeAll(const std::string& layerName, f32 minSlopeDeg, f32 maxSlopeDeg, f32 minZCm, f32 maxZCm, f32 featherDeg, f32 amount);
    // Drops layer `index` of one section (as removeLayer does for the primary).
    bool removeLayer(u32 section, u32 index);
    usize ringTileCount() const { return landscapeRingTiles_.size(); }
    bool ringTileResident(i32 tx, i32 ty) const { return landscapeRingTiles_.count(landscape::TileCoord{tx, ty}) != 0; }

    // The ring/noise parameters an authoring UI edits in place -- also what the ring tiles and
    // scatterHeightAt's past-the-rim fallback read.
    landscape::TerrainNoiseParams& noiseParams() { return landscapeNoiseParams_; }
    const landscape::TerrainNoiseParams& noiseParams() const { return landscapeNoiseParams_; }

    // True once heights, paint or origin of ANY section have been edited since its last load or save.
    bool dirty() const;

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
    // Primary section only; see applyHeightRect(section, ...) for the others.
    bool applyHeightRect(u32 x0, u32 y0, u32 x1, u32 y1, const std::vector<f32>& src);

    // Drains the invalidate applyHeightRect latches: when pending, clears it and, with both a
    // renderer and a device, forgets the GPU meshes overlapping the latched rect. The GPU half is
    // deferred a frame on purpose -- applyHeightRect (reached from undo/redo) runs in places with no
    // device to hand, while the heightfield and quadtree it edits update synchronously so queries see
    // the change immediately; only the cached GPU meshes lag by a frame, invisibly.
    void flushPendingInvalidate(rhi::IDevice* device);

    // Starts a sculpt stroke (on every section): nothing touched yet. The pre-stroke heights are captured lazily as the
    // rect grows (see growStroke), not up front -- at stroke start the rect a drag will end up
    // covering is not known yet.
    void beginStroke();

    // Unions this tick's touched rect into the stroke's, capturing the pre-stroke heights of anything
    // newly covered. Must run BEFORE the tick's own height edit, or the "before" it captures is
    // already the "after". A no-op with an empty rect or no stroke active.
    void growStroke(const landscape::BrushRect& r);

    bool strokeActive() const { return strokeActive_; }

    // Ends a stroke and reports what changed in the PRIMARY section (endStroke(std::vector<HeightEdit>&) reports every section).
    // Returns false (and clears the captured buffer) when no
    // stroke was active, when nothing was ever touched, or when nothing is loaded. Otherwise fills
    // `after` from the section over the touched rect, moves the captured pre-stroke heights into
    // `before`, sets [x0,y0]-[x1,y1] to the touched rect, and returns true -- unless `before` and
    // `after` come out equal (a stroke that changed nothing, e.g. clicking on terrain already at the
    // flatten target), in which case the buffer is cleared and this returns false too. The caller
    // owns turning a true return into an undo entry.
    bool endStroke(std::vector<f32>& before, std::vector<f32>& after, u32& x0, u32& y0, u32& x1, u32& y1);

    // One sculpt tick on every section the brush reaches (the returned rect is the first touched section's): grows the active stroke over the brush's footprint, applies it at `amount`
    // (0..1, how much of the mode's per-tick strength to spend this tick), and -- if anything was
    // touched -- rebuilds the tree, widens skirts, resets hysteresis, and (with both a renderer and a
    // device) forgets the touched GPU meshes immediately, unlike applyHeightRect: a live brush has a
    // device on hand every tick, so there is nothing to defer. Marks dirty whether or not the rebuild
    // succeeded. Returns the touched rect (empty if the brush touched no sample, in which case
    // nothing else above runs).
    landscape::BrushRect sculpt(rhi::IDevice* device, const landscape::BrushParams& p, f32 amount);

    // Writes the primary section back to path() and every extra that is dirty to its own file (a failing file does not stop the
    // others; false and `why` then name the first failure). Returns false when nothing is loaded, path() is
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
        u64 lastDrawn = 0;   // GameLandscape::drawFrame_ it last drew on (0 = no meshes held)
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

    // ---- wave3 layers ----
    // Weightmap paint (the primary-only forms below, and the section-aware ones in "sections"), the terrain material's layer table, physical-surface queries and the
    // GPU weight textures (docs/LANDSCAPE_LAYERS_WATER.md). Paint is stored in the .ocland by layer NAME; the material's
    // layers are matched to it by name, so reordering the material never scrambles paint. Everything but the GPU upload
    // (flushWeights) is CPU only and works headless.

    // One paint layer of the terrain material as plain data, so this header needs no PBR include.
    struct LayerInfo {
        std::string name, surface, foliage;
        f32 friction = 0.6f, restitution = 0.0f;
        f32 tilingCm = 400.0f, foliageDensity = 0.0f, foliageMinWeight = 0.5f;
        const std::string& surfaceName() const { return surface.empty() ? name : surface; }
    };
    // What sampleGround answers. weights are in MATERIAL layer order and sum to 1.
    struct GroundInfo {
        f32 z = 0.0f;
        f32 normal[3] = {0.0f, 0.0f, 1.0f};
        f32 weights[8] = {};
        u32 dominant = 0;
        f32 friction = 0.6f, restitution = 0.0f;
        std::string surface;
    };

    // The material's layers (up to 8, the first is the base). A host without the PBR module, or a test, sets the table
    // directly; with PBR it follows the LANDSCAPE material automatically.
    void setLayerTable(std::vector<LayerInfo> layers);
    u32 materialLayerCount() const { return static_cast<u32>(layerTable_.size()); }
    const std::string& materialLayerName(u32 i) const;
    const LayerInfo* materialLayer(u32 i) const { return i < layerTable_.size() ? &layerTable_[i] : nullptr; }
    // True while the LANDSCAPE material is a layered terrain material (its layer table is live).
    bool terrainMaterialActive() const { return !layerTable_.empty(); }

    // Finds or adds the section layer called `name` (the first one added to an unpainted section is preceded by the
    // material's base layer, so layer 0 is always the base). Call BEFORE a stroke: a stroke records a fixed plane count.
    // False with no table, at 8 layers, or while a weight stroke is active. Marks dirty.
    bool ensureLayer(const std::string& name, u32& outIndex);
    // Drops section layer `index` (>= 1), folding its paint into the base. False during a weight stroke.
    bool removeLayer(u32 index);
    // Whole-section edits. Run them inside beginWeightStroke / endWeightStroke to get one undo entry.
    void fillLayer(u32 layer);
    void autoFillBySlope(u32 layer, f32 minSlopeDeg, f32 maxSlopeDeg, f32 minZCm, f32 maxZCm, f32 featherDeg, f32 amount);

    // One brush tick: grows the weight stroke first, applies it, marks the weights dirty. Heights, the quadtree and
    // collision are untouched. Returns the touched rect (empty if nothing was).
    landscape::BrushRect paint(rhi::IDevice* device, const landscape::PaintParams& p, f32 amount);

    // The weight stroke mirrors the height stroke: before-planes captured lazily over the unioned rect, one undo entry per
    // drag. endWeightStroke returns false (nothing to undo) when nothing changed. `planes` = stored layers at stroke time.
    void beginWeightStroke();   // on every section
    void growWeightStroke(const landscape::BrushRect& r);
    bool weightStrokeActive() const { return wStrokeActive_; }
    bool endWeightStroke(std::vector<u8>& before, std::vector<u8>& after, u32& x0, u32& y0, u32& x1, u32& y1, u32& planes);
    // Undo/redo: writes `planes` stored planes of a rect back. False for no section, a bad rect or more planes than exist.
    bool applyWeightRect(u32 x0, u32 y0, u32 x1, u32 y1, u32 planes, const std::vector<u8>& src);
    bool weightsDirty() const { return edit_.weightsDirty; }

    // Height, slope, paint weights and physical surface at a world XY (CPU, headless). False outside every section.
    bool sampleGround(f32 xCm, f32 yCm, GroundInfo& out) const;
    // Weight-averaged friction and restitution only; allocation-free and a pure read, because the physics surface sampler
    // calls it on worker threads (paint is not edited while a Play session or Simulate runs).
    bool surfaceFrictionAt(f32 xCm, f32 yCm, f32& outFriction, f32& outRestitution) const;

    // Rebuilds the weight textures and the section's parameter texture and swaps them into the section material, at most
    // every 100 ms while a stroke is down and at once when it ends. Call every frame before drawing (draw() does).
    // A no-op without a device, the PBR module or a layered terrain material.
    void flushWeights(rhi::IDevice* device, pbr::MaterialSystem* materials);

    // One-shot carve commands (a normal height stroke for undo): lower the heights along a river centreline or inside a lake
    // outline. Same tail as sculpt(): grows the stroke, rebuilds the tree, drops overlapping GPU meshes, marks collision stale.
    landscape::BrushRect carveRiver(rhi::IDevice* device, const landscape::CarvePoint* pts, u32 n, const landscape::CarveParams& p);
    landscape::BrushRect carveBasin(rhi::IDevice* device, const f32* polyXY, u32 count, f32 levelCm, const landscape::CarveParams& p);

#if AVER_MODULE_PBR
    // The two materials a layered terrain draws through, derived from the authored one: the section's copy names the
    // runtime weightmaps ("@rt:land.w0" / "land.w1") and parameter texture ("@rt:land.params") in slots 3..5; the ring
    // tiles' copy has no "@rt:" refs at all, so every pixel of a ring tile is the base layer.
    // `rtSuffix` is appended to the three runtime names, so each extra section binds its own weightmaps.
    static void makeTerrainSectionDescs(const pbr::MaterialDesc& base, pbr::MaterialDesc& section, pbr::MaterialDesc& ring,
                                        const std::string& rtSuffix = std::string());
#endif

    // Changes the LANDSCAPE material by name (the editor's material picker); the next draw rebinds. The caller records the name
    // in the level's LANDSCAPE record.
    void setSurfaceMaterial(const std::string& name);

    // Installs the framework's terrain provider (aver_fw_set_terrain_provider) so C# and graphs can query layers. One call
    // per host beside GameWater::init. A no-op without the framework module.
    void installProviders();
    // ---- end wave3 layers ----

private:
    // Floating origin: moves the frame the section, ring tiles and renderers live in. See onOriginShift.
    void moveFrameTo(const rhi::ShiftOrigin& o);
    // Puts a section just read from disk (absolute f32 originCm, == homeAbs_) into the current frame.
    void placeInFrame(fmt::OcLandData& d) const;

    // ---- authoring state, one per section ----
    // Everything an edit needs to remember about a section besides its data: unsaved work, stale collision, the GPU meshes an undo
    // still has to drop, and the section's share of the height / weight stroke in progress.
    struct SectionEdit {
        bool dirty = false;            // see dirty()
        // Heights have moved since the collision body was last built. SEPARATE FROM dirty on purpose: dirty answers "does the
        // user have unsaved work" and is cleared by saving, this answers "does the physics shape still describe the mesh" and
        // is cleared by rebuilding. See rebuildCollisionIfStale().
        bool collisionStale = false;
        bool weightsDirty = false;     // the weight textures must be rebuilt
        // Latched by applyHeightRect, drained by flushPendingInvalidate.
        bool pendingInvalidate = false;
        u32  pendingX0 = 0, pendingY0 = 0, pendingX1 = 0, pendingY1 = 0;
        // This section's share of the sculpt stroke (the stroke itself, strokeActive_, is shared by all sections).
        bool strokeEmpty = true;
        u32  strokeX0 = 0, strokeY0 = 0, strokeX1 = 0, strokeY1 = 0;
        std::vector<f32> strokeBefore;
        // ... and of the weight stroke.
        bool wEmpty = true;
        u32  wX0 = 0, wY0 = 0, wX1 = 0, wY1 = 0, wPlanes = 0;
        std::vector<u8> wBefore;
    };
    // Pointers to the parts of one section, so the authoring code is written once for the primary and the extras.
    struct SecRef {
        fmt::OcLandData* data;
        landscape::LandscapeTree* tree;
        std::unique_ptr<landscape::LandscapeRenderer>* renderer;
        SectionEdit* edit;
        std::array<i32, 8>* remap;
        f64* homeAbs;
        const std::string* path;
    };
    SecRef ref(u32 section);   // 0 = primary
    void growHeightStroke(SecRef s, const landscape::BrushRect& r);
    void growWeightStrokeIn(SecRef s, const landscape::BrushRect& r);
    bool applyHeightRectIn(SecRef s, u32 x0, u32 y0, u32 x1, u32 y1, const std::vector<f32>& src);
    bool applyWeightRectIn(SecRef s, u32 x0, u32 y0, u32 x1, u32 y1, u32 planes, const std::vector<u8>& src);
    // The tail of a height edit on one section: bounds, tree rebuild, GPU meshes, dirty and stale collision.
    void sectionHeightsChanged(SecRef s, rhi::IDevice* device, const landscape::BrushRect& touched);
    // One edit over every section: `rectOf` is the footprint (grown into the stroke before `apply` writes), `apply` the edit.
    // Shared seam samples are then made equal. Returns the first touched section's rect.
    landscape::BrushRect editHeights(rhi::IDevice* device,
                                     const std::function<landscape::BrushRect(const fmt::OcLandData&)>& rectOf,
                                     const std::function<landscape::BrushRect(fmt::OcLandData&)>& apply);
    // Makes samples that two sections share (same world position) equal, for the ones a stroke touched; `touched` (per section)
    // grows to cover what this changed. Heights average; weights average per layer name.
    void matchSeams(std::vector<landscape::BrushRect>& touched, bool weights);
    // Adds layer `name` to a section (also mid-stroke, for a section that has captured nothing yet). False without a layer table.
    bool ensureLayerOn(SecRef s, const std::string& name, u32& outIndex);
    void refreshWeightPlanes(SecRef s);
    bool saveSection(u32 section, std::string* why);
    struct Section;
    void rebuildExtraBody(Section& e);

    // Loads one .ocland section and builds its quadtree over it. Pure CPU. Calls unload() first, so
    // `device` frees whatever section is currently resident before this one replaces it (null is only
    // safe when nothing is resident yet). Clears dirty_ -- a freshly loaded section has nothing
    // pending to save.
    bool loadLandscape(rhi::IDevice* device, const std::string& path);

    // One extra section: the .ocland at `path`, placed at `at` (absolute) when `haveAt`.
    bool loadExtra(rhi::IDevice* device, const std::string& path, bool haveAt, const f64 at[3]);
    // Puts a section at absolute (x, y) [and z when given]: its scene-frame origin and xy bounds.
    void placeSection(fmt::OcLandData& d, const f64 absXY[2], const f64* atZ) const;
    // The section holding scene-frame (x, y): the primary or an extra. False outside all of them.
    struct Hit { const fmt::OcLandData* data = nullptr; const std::array<i32, 8>* remap = nullptr; f32 z = 0.0f; };
    bool findSection(f32 xCm, f32 yCm, Hit& out) const;
    // A static heightfield body for `d` (<= 0 on failure), its paint layers mapped through `remap`.
    i32 buildBody(const fmt::OcLandData& d, const std::array<i32, 8>& remap) const;
    void rebuildExtraCollision();

    // Rebuilds the resident section's static collision body from scratch (Jolt's heightfield shape
    // is immutable once created -- there is no partial update). Called from loadForLevel and save()
    // only -- NEVER per sculpt tick, per undo/redo, or on generating fresh noise, which would rebuild
    // it far more often than any of those actually needs collision to be current. A no-op without
    // AVER_MODULE_PHYSICS.
    void rebuildCollision();

    // Registers every material layer's surface with the physics module (find-or-add: an authored Surfaces.ocsurf entry of the
    // same name keeps its numbers). Called when the layer table is set and before a collision rebuild.
    void registerLayerSurfaces();


    std::function<void()> terrainChanged_;   // see setTerrainChangedHook

#if AVER_MODULE_PBR
    // Pushes the level's LANDSCAPE material into one renderer, as an opaque binding, and resolves the
    // UV tiling from it. The host (not this module) does the resolving because the material system's
    // own readiness and the level load race each other with no guaranteed order; the tiling has to be
    // re-asked for with pbr::MaterialLibrary::get().valid(h) rather than trusted on the first
    // constants() call, since constants() itself returns a fallback for any handle not yet valid.
    // `sectionMat` (extras) replaces the primary's derived material for a section draw.
    void applySurface(landscape::LandscapeRenderer& r, GameContent& content, pbr::MaterialSystem& materials, bool ring = false,
                      u32 sectionMat = 0);
    // wave3 layers: finds the LANDSCAPE material, and for a layered terrain material keeps its derived section / ring
    // materials, layer table and remap current. True when anything changed (the renderers must rebind).
    bool syncTerrain(GameContent& content, pbr::MaterialSystem& materials);
    // Gives the derived materials and runtime textures back (textures are destroyed now when `device` is given, else
    // retired and destroyed by a later flushWeights).
    void releaseTerrain(rhi::IDevice* device);
    // Builds one section's weight textures (and its parameter texture unless kept) and swaps them in under the runtime
    // names "land.w0<suffix>" ...; the replaced ones are retired. Shared by the primary and every painted extra.
    void uploadSectionWeights(rhi::IResourceFactory& res, pbr::MaterialSystem& materials, const pbr::MaterialDesc& base,
                              const pbr::TerrainTextures& info, const fmt::OcLandData& data, const std::array<i32, 8>& remap,
                              const std::string& rtSuffix, u32& texA, u32& texB, u32& texParams, bool& paramsDirty);
    // Creates or updates the derived material of every painted extra section from the base terrain desc.
    void syncExtraMaterials(const pbr::MaterialDesc& base);
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

    // A further LANDSCAPE record's section: what rendering, collision and queries need of the primary.
    struct Section {
        fmt::OcLandData data;
        landscape::LandscapeTree tree;
        std::unique_ptr<landscape::LandscapeRenderer> renderer;
        std::string path;
        f64 homeAbs[2] = {0.0, 0.0};
        i32 body = -1;
        std::array<i32, 8> remap{};   // this section's paint layer i -> material layer, like sectionToMaterial_
        u64 lastDrawn = 0;            // drawFrame_ it last drew on
#if AVER_MODULE_PBR
        u32 mat = 0, texA = 0, texB = 0, texParams = 0;   // derived material and runtime textures (painted sections)
        bool paramsDirty = true;
#endif
        SectionEdit edit;
    };
    std::vector<Section> extras_;
    u64 drawFrame_ = 0;
    u64 primaryLastDrawn_ = 0;

    // ---- authoring state ----
    std::string pathOverride_;   // see setPathOverride
    SectionEdit edit_;           // the primary's; each extra holds its own
    // The shared stroke flags (a stroke spans sections); each section's captured share is in its SectionEdit.
    bool strokeActive_ = false;

    fmt::OcLandData landscapeData_;
    // The scene origin the section, the ring tiles and their renderers are expressed in (zero until a host names one).
    rhi::ShiftOrigin frame_{};
    // The resident section's sample (0,0) in ABSOLUTE cm (x, y), exact: the file's f32 or the LANDSCAPE record's f64 `at`.
    // originCm in landscapeData_ is f32(homeAbs_ - frame_), one rounding however many rebases have happened, and save()
    // writes homeAbs_ back so a flight never drifts the sidecar.
    f64 homeAbs_[2] = {0.0, 0.0};
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
    // Many sections: the nearest few in view draw, sharing one total nearest first (each gets at least
    // kMinDrawsPerSection), so the mesh cache stays near kMaxDrawnSections * kMaxResidentNodesPerTile.
    static constexpr u32 kMaxDrawnSections = 16;
    static constexpr u32 kTotalDraws = 192;
    static constexpr u32 kMinDrawsPerSection = kTotalDraws / kMaxDrawnSections;
    static constexpr f32 kDrawRadiusCm = 300000.0f;      // horizontal distance from the camera to a footprint
    static constexpr u64 kIdleFramesBeforeForget = 300;  // not drawn for this long: its GPU meshes are dropped
    landscape::TileCoord lastCameraTile_{};
    bool lastCameraTileValid_ = false;

    // ---- wave3 layers ----
    void rebuildRemap();
    void remapFor(const fmt::OcLandData& d, std::array<i32, 8>& out) const;
    void updateSurfaceSampler();
    // The tail of a height edit that is not a brush tick (carve): tree rebuild, GPU meshes, collision, dirty.
    void heightsEdited(rhi::IDevice* device, const landscape::BrushRect& touched);
    static i32 surfaceThunk(f32 x, f32 y, f32* outFriction, f32* outRestitution, void* user);

    std::vector<LayerInfo> layerTable_;                // the material's layers (empty = no layered terrain)
    std::array<i32, 8> sectionToMaterial_{};           // section layer i -> material layer (-1 = folds into the base)
    bool wStrokeActive_ = false;
#if AVER_MODULE_PBR
    // Derived materials: the base terrain material copied with the section's weightmaps ("@rt:land.*") and a plain copy
    // for the ring tiles (no weightmaps: every pixel is the base layer). Handles are pbr::MaterialHandle.
    u32 baseMat_ = 0, secMat_ = 0, ringMat_ = 0;
    u64 baseSig_ = 0;
    bool terrainActive_ = false;
    bool paramsDirty_ = true;                          // the primary's parameter texture must be rebuilt (layers or section changed)
    pbr::MaterialSystem* lastMaterials_ = nullptr;
    u32 texA_ = 0, texB_ = 0, texParams_ = 0;          // rhi::TextureHandle of the live runtime textures (0 = none)
    struct Retired { u32 texture; u64 frame; };
    std::vector<Retired> retired_;                     // replaced textures, destroyed a few frames after the swap
    u64 flushFrame_ = 0;
    std::chrono::steady_clock::time_point lastUpload_{};
#endif
    // ---- end wave3 layers ----
};

} // namespace aver::game

#endif // AVER_MODULE_LANDSCAPE
