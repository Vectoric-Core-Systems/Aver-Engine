// GameWater: level-authored water -- the analytic Gerstner surface (WaterRenderer) plus its
// buoyancy plane, and the simulated soft-body fluid volume (FluidScene) a WATER record may ask
// for instead. Shared by both hosts: GameApp drives it through GameLevel's LoadHooks like
// GameLandscape, and the editor's SandboxApp drives the same instance through applyLevel plus the
// editor-only entry points below (attachSurface, respawnLevelVolume) -- see each method's own
// comment for what it does.
//
// Takes a device and the level's OcWorldData as arguments rather than reaching into GameApp for them.
// Each host owns one by value.
//
// LEFT OUT, EDITOR-ONLY:
//   - the Details panel's live water edit (SandboxPanels.cpp) -- stays editor-side, re-applying
//     through applyLevel on every edit.
//   - the --water CLI flag and its onInit registration -- stays editor-side, bringing a surface up
//     through attachSurface.
//   - re-settling a level's water when a play session resets (the editor's Stop) -- goes through
//     respawnLevelVolume.
//   - the Player Start-style viewport icon and Outliner entry a spawned volume gets in the editor
//     (viewportIcons_/playerStartIcon_) -- decoration for a human editing, not gameplay.
//   - cbStatus_ notification strings.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/formats/OcWorld.hpp"

#if AVER_MODULE_FLUIDS
#include "aver/fluids/WaterRenderer.hpp"
#include "aver/fluids/Underwater.hpp"
#include "aver/fluids/GerstnerWave.hpp"

#if AVER_FLUIDS_SIMULATED
#include "aver/fluids/FluidScene.hpp"
#include "aver/fluids/FluidVolume.hpp"
#endif

#include <string>
#include <unordered_map>
#include <vector>

// Forward-declared unconditionally, matching GameRender.hpp's own drawWorld and GameLandscape.hpp's
// own applySurfaceToAll: draw() below takes a pbr::MaterialSystem* even in a build with the PBR
// module absent, where it is always null and never dereferenced (every real use is inside an
// `#if AVER_MODULE_PBR` block in GameWater.cpp).
namespace aver::pbr { class MaterialSystem; }

namespace aver::game {

class GameContent;

class GameWater {
public:
    // Registers the simulated fluid scene (FluidScene) and the framework's fluid-spawn relay, so a
    // graph-authored `COMP ... Fluid` or a script's spawn call works even before any level asks for
    // water. Call once, BEFORE any level loads and before Voxi attaches (Voxi's
    // acceleration-structure build reads the vertex buffer this feature's prePass writes).
    void init(rhi::IDevice& device);

    // Turns a level's WATER/WAVE records (`w.waters`/`w.waves`) into a rendered analytic surface +
    // buoyancy plane, or into a latched simulated-volume spawn request. A no-op when `w.waters` is
    // empty. Called as the level loads, before its placements.
    void applyLevel(rhi::IDevice& device, const fmt::OcWorldData& w);

    // Brings the analytic surface up if it is not already registered (WaterRenderer::init +
    // device.addRenderFeature), then sets its level and waves -- for a caller that wants a surface
    // on screen outside a level's own WATER record (the editor's --water CLI flag). Leaves the
    // surface's bounds and the ripple set (device.setWaterWaves) untouched. Returns false, logging
    // nothing, if WaterRenderer::init fails -- the caller logs.
    bool attachSurface(rhi::IDevice& device, f32 levelCm, const fluids::GerstnerWave* waves, size_t n);

    // Despawns the level-authored simulated volume (fluidHandle_) and re-latches its spawn so
    // update() spawns it again fresh -- for a host that resets a play session (the editor's Stop).
    // Graph-authored volumes are untouched. Returns false when no level volume is live.
    bool respawnLevelVolume();

    // Drains a latched spawn request (from applyLevel) and every graph-authored one
    // (aver_fw_fluid_spawn / aver_fw_fluid_spawn_material), steps the simulated volume, and syncs
    // the player's buoyancy impulse against it, and advances the analytic surface's wave clock by
    // `dt`. Call once per frame, AFTER the physics step and BEFORE World::flush.
    void update(rhi::IDevice& device, f32 dt);

    // `sky` with its fog swapped for the underwater look while `cameraZCm` is below the rendered
    // analytic surface, otherwise `sky` returned unchanged -- applied to a copy so the authored sky
    // itself is never modified (a level saved mid-dive must not carry underwater fog as its own
    // weather).
    rhi::SkyAtmosphere applyUnderwaterFog(const rhi::SkyAtmosphere& sky, f32 cameraZCm) const;

    // Draws every live simulated volume as an ordinary blended mesh. `materials` may be null, the
    // same contract GameRender.hpp's drawWorld already uses for a game with no material system (or
    // Voxi not yet attached): the volume then draws with its fallback look. The analytic surface
    // needs no call here -- WaterRenderer draws itself through its own transparentPass once
    // attachSurface (directly, or through applyLevel) has registered it.
    void draw(rhi::IDevice& device, GameContent& content, pbr::MaterialSystem* materials) const;

    // Despawns what a level itself asked for: the latched analytic/simulated request and every
    // graph-authored volume. Leaves the analytic WaterRenderer's registration and last surface in
    // place. Call wherever the level unloads.
    void unload();

    // Unregisters and shuts down every render feature this class owns. `device` may be null,
    // meaning nothing is resident to unregister (mirrors GameLandscape::unload's own
    // device-scoped-teardown contract). Call once, from GameApp::onShutdown, BEFORE physics shuts
    // down (a live simulated volume's teardown calls into the solver).
    void shutdown(rhi::IDevice* device);

    // Whether the analytic surface is currently registered as a render feature.
    bool attached() const { return waterAttached_; }
    // The analytic surface's authored height, or 0 when nothing is attached.
    f32 waterLevelCm() const { return waterAttached_ ? waterRenderer_.waterLevelCm() : 0.0f; }

private:
#if AVER_FLUIDS_SIMULATED
    void drawOneFluid(rhi::IDevice& device, GameContent& content, pbr::MaterialSystem* materials,
                      fluids::FluidHandle h) const;

#if AVER_MODULE_FRAMEWORK
    // The framework's fluid-spawn relay (framework_abi.h's aver_fw_set_fluid_spawn_provider /
    // _material_provider) -- a graph-authored `COMP ... Fluid` or a script's Game.SpawnFluidVolume
    // call reaches fluidGraphQueue_ through these.
    static i32 fluidSpawnProvider(f32 cx, f32 cy, f32 cz, f32 hx, f32 hy, f32 hz,
                                  f32 compliance, f32 damping, i32 iterations, f32 pressure,
                                  const char* name, void* user);
    static i32 fluidSpawnMaterialProvider(f32 cx, f32 cy, f32 cz, f32 hx, f32 hy, f32 hz,
                                          f32 compliance, f32 damping, i32 iterations, f32 pressure,
                                          f32 densityKgM3, f32 viscosityPaS, const char* materialPreset,
                                          const char* name, void* user);
#endif
#endif

    // ---- the analytic Gerstner surface, its buoyancy plane, and the underwater fog tuning it drives ----
    fluids::WaterRenderer waterRenderer_;
    fluids::UnderwaterFogTuning waterFog_{};
    bool waterAttached_ = false;

#if AVER_FLUIDS_SIMULATED
    // ---- the simulated half: a soft-body volume, kept beside the analytic renderer rather than
    // inside it -- a WATER record is either a Gerstner surface or a soft body, never both. ----
    fluids::FluidScene fluidScene_;

    fluids::FluidHandle fluidHandle_ = 0;
    // What applyLevel's own WATER record asked for, and whether that ask is still outstanding --
    // latched rather than spawned immediately because FluidScene is not ready() until init()'s
    // render feature has actually come up, and applyLevel can run before that. update() drains it.
    fluids::FluidVolumeDesc fluidWantDesc_{};
    std::string fluidWantName_;
    bool fluidWantPending_ = false;
    // The surface .ocmat name applyLevel's WATER record carried, moved into fluidSurfaceMaterial_
    // below once update()'s drain has a handle to key it by.
    std::string fluidWantSurfaceMaterial_;

    // Graph-authored fluid requests, queued by fluidSpawnProvider/fluidSpawnMaterialProvider for
    // the identical readiness reason fluidWantPending_ is latched, and drained at the same point.
    struct FluidGraphRequest { fluids::FluidVolumeDesc desc; std::string label; };
    std::vector<FluidGraphRequest> fluidGraphQueue_;
    // Every handle the queue above has ever produced, so unload() can despawn all of them.
    std::vector<fluids::FluidHandle> fluidGraphHandles_;
    // Which .ocmat each live volume's surface uses, by NAME rather than a resolved
    // pbr::MaterialHandle -- a spawn can be drained before materials finish loading, so draw()
    // resolves the name fresh every call instead of caching a handle that could be a permanent zero.
    std::unordered_map<fluids::FluidHandle, std::string> fluidSurfaceMaterial_;

    // The player's world position last frame, and whether that reading is trustworthy -- there is
    // no direct read of the character's own physics velocity, so update() recovers it by finite
    // difference. Invalidated whenever a frame has no controlled pawn.
    Vec3 fluidPrevPlayerPosCm_{};
    bool fluidPrevPlayerValid_ = false;
#endif
};

} // namespace aver::game

#endif // AVER_MODULE_FLUIDS
