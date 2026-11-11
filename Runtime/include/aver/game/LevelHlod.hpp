// LevelHlod: far detail for a streamed authored level, generated while it runs (docs/LEVEL_STREAMING.md, HLOD).
//
// Every streaming cell with static meshes gets a merged proxy: the cell's placements baked into one mesh in their world poses,
// grouped by material (so every surface keeps its own material and textures), with what lies inside another item's bounds left out
// (interiors), then simplified into a LADDER of kLevels variants: level k keeps an error of `pixelError` pixels at loadCm * 2^k, and a
// cell shows the level its distance allows. That is the Voxy idea without voxels: distant content stays recognisable, its cost falls
// with distance, and it draws as one entity per cell instead of hundreds. A background worker builds the cells nearest the viewer
// first and keeps every level in <project>/Saved/DerivedDataCache/HLOD/<level>/, keyed by what went into it, so the next run reads
// instead of building; only the level on screen is in memory.
// PlacementStreamer's HLOD ring decides when a proxy shows: from hlodCm while none of its cell's content is resident, hidden once
// the content is complete or past hlodEvictCm. Proxies are ordinary mesh entities (drawn, shadowed and ray traced like any mesh, kept
// out of the voxel GI bake), never saved, never selectable (owns()), with no physics. Both hosts get them through LevelStreaming.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/OcWorld.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"
#  include "aver/world/PlacementStreamer.hpp"
#  include "aver/world/WorldOrigin.hpp"

namespace aver::rhi { class IDevice; }

namespace aver::game {

class GameContent;

class LevelHlod {
public:
    struct Settings {
        f32 cellCm = 6400.0f;
        f32 loadCm = 25000.0f;        // the streamer's load distance: the nearest a proxy is normally seen from is about loadCm - cellCm
        f32 hlodCm = 0.0f;            // proxies show out to here; 0 = off
        f32 hlodEvictCm = 0.0f;       // and hide past here (<= hlodCm: hlodCm + cellCm)
        f32 pixelError = 1.0f;        // geometric error allowed at that nearest distance, in pixels of a viewportPx-high view
        f32 viewportPx = 2160.0f;
        f32 fovYDeg = 60.0f;
    };
    // The level's HLOD settings: its STREAM hlod= when set (negative = off), else four load distances. AVER_HLOD=0 turns it off.
    static Settings settingsFor(const fmt::OcStreamSettings& stream);

    LevelHlod() = default;
    ~LevelHlod();
    LevelHlod(const LevelHlod&) = delete;
    LevelHlod& operator=(const LevelHlod&) = delete;

    // Plans the cells of `w` (its placements, as LevelStreaming holds them), hands them to `streamer` and starts the worker. The
    // streamer must already be built with s.hlodCm / s.hlodEvictCm. False (and inert) when the level has nothing to build.
    bool begin(const fmt::OcWorldData& w, GameContent& content, rhi::IDevice& device, world::PlacementStreamer& streamer,
               const Settings& s);
    // One step, after the streamer's own update: shows and hides proxies, uploads finished ones within a small time budget.
    // Viewers are absolute cm (the streamer's frame); new entities are placed at absolute - origin.
    void tick(scene::World& world, world::PlacementStreamer& streamer, const std::vector<world::Vec3d>& viewers,
              world::OriginCm origin);
    // Stops the worker, destroys every proxy and releases its mesh.
    void end(scene::World& world);
    bool active() const { return active_; }
    // A proxy entity (not content: never saved or selected).
    bool owns(scene::Entity e) const { return entities_.count(static_cast<u32>(e)) != 0; }

    usize cellCount() const { return cells_.size(); }
    usize shownCount() const { return entities_.size(); }

    // The geometric error a proxy may have, in cm: `pixelError` pixels at `distanceCm` in a `viewportPx`-high, fovYDeg view.
    static f32 errorCmAt(f32 distanceCm, const Settings& s);

    static constexpr u32 kLevels = 4;
    // The ladder level a cell `distanceCm` away shows (0 finest), with hysteresis around the current one.
    static u32 levelFor(f64 distanceCm, f32 loadCm, i32 current);

private:
    enum class State : u8 { Unbuilt, Building, Built, Empty };
    struct Cell {
        i32 cx = 0, cy = 0;
        std::vector<u32> placements;
        // Worker side (under mutex_).
        State state = State::Unbuilt;
        std::string files[kLevels];             // Built: each level's cache file
        i32 loadLevel = -1;                     // a level the main thread asked the worker to read
        std::unique_ptr<fmt::OcMeshData> loaded;   // ...and what it read
        i32 loadedLevel = -1;
        // Main thread.
        bool wanted = false;                    // the ring shows it
        i32 shownLevel = -1;
        scene::Entity entity = scene::kInvalidEntity;
        u64 meshId = 0;
    };
    void work();
    bool buildCell(u32 index, bool& fromCache);
    void show(scene::World& world, Cell& c, fmt::OcMeshData& mesh, i32 level, world::OriginCm origin);
    void despawn(scene::World& world, Cell& c);

    bool active_ = false;
    GameContent* content_ = nullptr;
    rhi::IDevice* device_ = nullptr;
    Settings s_{};
    f32 errorCm_[kLevels] = {};
    std::string level_, contentDir_, cacheDir_;
    std::vector<fmt::OcWorldPlacement> records_;   // the asset, material and per-placement matrix the worker reads
    std::vector<f64> matrices_;
    std::vector<Cell> cells_;
    std::unordered_map<u32, u32> entities_;        // proxy entity raw -> cell

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    world::Vec3d viewer_{};                        // the worker builds nearest this first (absolute cm)
    bool haveViewer_ = false;
    u32 built_ = 0, cached_ = 0, failed_ = 0;
    bool reported_ = false;
};

} // namespace aver::game

#endif // AVER_MODULE_SCENE
