// GameFoliage: loading a level's instanced foliage into the renderer, for both hosts.
//
// A level's instanced foliage lives OUTSIDE the entity/draw system entirely -- see
// modules/render.voxi/include/aver/voxi/VoxiRenderer.hpp's FoliagePrototype/FoliageInstance/
// setFoliage for the shape it lands in: static, ray-traced only (TLAS), never in
// VoxiRenderer::draws_, no collision, not individually selectable. It is a LOAD, once per level
// open, not a placement loop -- the reason this is one function rather than a scene::World walk.
//
// SHARED BY THE EDITOR AND THE RUNTIME, the same way GameLevel/GameRender already are: both call
// this from their own GameLevel::LoadHooks::afterInstantiate, after placements are instantiated,
// with their own GameContent and voxi::VoxiRenderer.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"

#include <functional>
#include <string>
#include <vector>

#if AVER_MODULE_SCENE && AVER_MODULE_VOXI
#include "aver/formats/OcWorld.hpp"

#include "aver/formats/OcInstances.hpp"

namespace aver::voxi { class VoxiRenderer; }
namespace aver::rhi { class IDevice; }

namespace aver::game {

class GameContent;

// What one call to loadLevelFoliage did, for the caller's own summary log line -- mirrors
// world::LevelInstance's own "counted here because only this loop knows" reasoning.
struct FoliageLoadResult {
    u32 files = 0;          // FOLIAGE records that resolved to a path and parsed
    u32 groups = 0;         // OcInstanceGroup entries carried into a FoliagePrototype, across every file
    u32 instances = 0;      // total instances handed to voxi::VoxiRenderer::setFoliage
    u32 droppedParts = 0;   // translucent parts dropped (Voxi never receives a blended instance)
    // Non-empty when at least one FOLIAGE file could not be read or parsed at all (the reason
    // named is the LAST such failure; each is also logged individually as it happens). A missing
    // mesh or an empty level are warnings only, and do not set this -- they are not why the level
    // failed to load its foliage, they are content gaps this function already reports on its own.
    std::string error;
};

// Loads every FOLIAGE file `w.foliageFiles` names into `voxi` (voxi::VoxiRenderer::setFoliage), or
// clears whatever was there when the level names none. Each group's mesh and material are resolved
// EXACTLY the way game::drawWorld's resolveDrawLook does (Runtime/src/GameRender.cpp, the
// authored > dead-handle > named-look > flat-fallback ladder), through the same shared
// aver::game::resolveSurfaceLook (SceneSubmission.hpp) that lambda calls -- see this function's own
// definition for the one place it has to replicate resolveDrawLook's TWO GameContent lookups by
// hand rather than call it directly (it is a closure private to GameRender.cpp, not a free function).
//
// `contentDir` is the project's content root, against which each FOLIAGE record's content-relative
// path resolves -- the same convention GameContent::resolveMaterialGraph's GRAPHREF already uses.
// `progress`, when given, is called with a fraction in [0, 1] as files finish loading; the caller
// rescales it into whatever band its own loading screen reserves for foliage (see
// sandbox/src/SandboxLevelLoad.cpp / Runtime/src/GameApp.cpp for the two hosts' own bands).
//
// A NULL `voxi` is a legal no-op (every field 0, nothing read or written): a host with no attached
// Voxi renderer -- AVER_MODULE_VOXI compiled in but the feature never attached -- has nowhere to
// hand instances to, and asking it to would be reaching through a null pointer for nothing.
FoliageLoadResult loadLevelFoliage(const fmt::OcWorldData& w, GameContent& content,
                                    voxi::VoxiRenderer* voxi, const std::string& contentDir,
                                    const std::function<void(f32 fraction)>& progress = {});

// Foliage with distance residency (docs/LEVEL_STREAMING.md section 4). Keeps the parsed instance
// tables on the CPU. Without cells, or with streaming off, or with no device, load() pushes
// everything once (same as loadLevelFoliage). With cells, update() keeps only cells within the
// level's loadCm of the viewer (evicted past evictCm) and re-pushes through setFoliage when that
// set changes, at most every kCheckSeconds; prototype meshes are acquired/released through
// GameContent. setFoliage keeps prototype BLASes but re-sets the TLAS static prefix per call, so the
// throttle matters. The objects must outlive this; the renderer's foliage is
// NOT cleared by the destructor, only by clear().
class LevelFoliage {
public:
    static constexpr f32 kCheckSeconds = 0.25f;
    // Cells within loadCm * this have their prototype meshes read ahead (GameContent::prefetchMesh).
    static constexpr f32 kPrefetchFactor = 1.3f;
    static constexpr f64 kAcquireMs = 6.0;   // GPU uploads of a change's new meshes, per frame

    // `device` null: meshes are not acquired and every cell is resident (non-streamed behaviour).
    // `tablePaths`: absolute paths parallel to w.foliageFiles; a non-empty entry replaces
    // contentDir/<foliageFile> (the cell-sorted copy under Binaries/Streaming).
    // Every emitted group's mesh is acquired while `device` is given (cell-less files: until clear()).
    // `viewerCm` seeds the first residency so the initial push is already the right set.
    bool load(const fmt::OcWorldData& w, GameContent& content, rhi::IDevice* device,
              voxi::VoxiRenderer* voxi, const std::string& contentDir,
              const std::function<void(f32 fraction)>& progress = {}, const Vec3& viewerCm = {},
              const std::vector<std::string>* tablePaths = nullptr);
    void update(const Vec3& viewerCm, f32 dt);
    void clear();   // clears the renderer's foliage and releases held meshes

    bool streamed() const { return streamed_; }
    // Load / unload distances changed in place; the next check applies them.
    void setDistances(f32 loadCm, f32 evictCm) {
        loadCm_ = loadCm;
        evictCm_ = evictCm > loadCm ? evictCm : loadCm;
        accum_ = kCheckSeconds;
    }
    u32 totalCells() const { return static_cast<u32>(cells_.size()); }
    u32 residentCells() const { return residentCount_; }
    u32 instances() const { return result_.instances; }       // in the last push
    u32 rebuilds() const { return rebuilds_; }
    f32 lastRebuildMs() const { return lastRebuildMs_; }
    const FoliageLoadResult& result() const { return result_; }

private:
    struct File {
        fmt::OcInstanceData data;
        std::string path;
        std::vector<u64> objectIds;     // per group, fnv1a64(asset)
        std::vector<u32> use;           // per group: resident cells referencing it
        std::vector<char> held;         // per group: mesh acquired
        std::vector<char> warned;       // per group: missing-mesh warning issued
    };
    struct CellRef { u32 file, cell; };

    void rebuild();
    void applyResidency(const std::vector<char>& next);

    GameContent* content_ = nullptr;
    rhi::IDevice* device_ = nullptr;
    voxi::VoxiRenderer* voxi_ = nullptr;
    std::vector<File> files_;
    std::vector<CellRef> cells_;
    std::vector<char> resident_;
    u32 residentCount_ = 0;
    f32 loadCm_ = 0, evictCm_ = 0;
    f32 accum_ = 0;
    bool streamed_ = false;
    bool loaded_ = false;
    u32 rebuilds_ = 0;
    f32 lastRebuildMs_ = 0;
    FoliageLoadResult result_;
};

} // namespace aver::game

#endif // AVER_MODULE_SCENE && AVER_MODULE_VOXI
