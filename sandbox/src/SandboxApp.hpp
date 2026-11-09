#pragma once
// SandboxApp: the Aver editor executable. Viewport, gizmos, panels, Content Browser,
// and the frame loop that drives the runtime modules.

#include "aver/runtime/Engine.hpp"
#include "aver/runtime/Application.hpp"
#include "aver/platform/Window.hpp"
#include "aver/platform/Splash.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/platform/Image.hpp"
#include "aver/platform/DirectoryWatcher.hpp"
#include "aver/platform/InputState.hpp"
#include "aver/rhi/ShaderFiles.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/PrimitiveMeshes.hpp"
#include "aver/rhi/ShaderCacheSweep.hpp"
#include "aver/core/Log.hpp"
// Shared runtime content component.
#include "aver/game/GameContent.hpp"
#include "aver/game/SoftBodySave.hpp"   // soft-body damage on the $State channel
#include "aver/game/GameLevel.hpp"
#include "aver/game/GameWater.hpp"
#include "aver/game/GameStreaming.hpp"
#include "aver/game/GameFoliage.hpp"
#include "aver/game/GameLandscape.hpp"
#include "aver/game/MouseCapture.hpp"
#include "aver/game/PlayMobility.hpp"
#include "aver/game/OriginParticipants.hpp"

#include "aver/game/GameCamera.hpp"   // PlayView
#include "aver/game/SubViewportHost.hpp"   // G4 R5: sub-viewports
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS
#include "aver/world/VehicleSystem.hpp"
#endif
#include "aver/core/CrashReport.hpp"
#include "aver/core/Assert.hpp"
#include "aver/core/Math.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Version.hpp"
#include "aver/formats/OcBeam.hpp"
#include "aver/formats/OcMap.hpp"
#if AVER_WITH_AUDIO_ABI
// Set when Aver.Audio.Abi links in. Not renamed: tests/editor relies on it.
#  include "aver/audio/audio_abi.h"
#endif
#include "aver/formats/OcProject.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/formats/OcMesh.hpp"
// Behaviour-tree format (.ocbt).
#include "aver/formats/OcBt.hpp"
#include "aver/formats/GltfImport.hpp"
#include "ModelingMode.hpp"          // EditorMode::Modeling (W5 step 6)
#include "PcgGraphEditor.hpp"      // `DOMAIN pcg` graph tab (W3 step 6)
#include "HlodBuildJob.hpp"       // World Settings > Level Streaming > Build HLOD
#include "DataLayersPanel.hpp"   // Window > Data Layers (W10 step 4)
#include "ModelImportDialog.hpp"   // ModelImportSummary, writeImportedModel, importFbxToDir (FBX import)
#if AVER_MODULE_TRIFACTOR
#include "aver/trifactor/ClusterAdapt.hpp"
#endif
// Quadtree-LOD heightfield renderer.
#if AVER_MODULE_LANDSCAPE
#include "aver/formats/OcLand.hpp"
#include "aver/formats/OcFoliage.hpp"
#include "aver/landscape/LandscapeTree.hpp"
#include "aver/landscape/LandscapeRenderer.hpp"
// Terrain sculpt tools.
#include "aver/landscape/HeightfieldRay.hpp"
#include "aver/landscape/Sculpt.hpp"
// Terrain physics.
#include "aver/landscape/PhysicsBridge.hpp"
// Procedural terrain tiles around streamed sections.
#include "aver/landscape/TerrainTile.hpp"
#endif
#if AVER_HAVE_AUDIO_IMPORT
#  include "aver/formats/OcAudio.hpp"
#endif
#include "aver/ui/UiDrawList.hpp"
#include "aver/render/ui/UiRenderer.hpp"
#include "aver/render/SkinSelfTest.hpp"
#include "aver/pt/PtFurnaceTest.hpp"
#include "aver/pt/PtSceneView.hpp"
#include "SkinDrawTest.hpp"
#include "SkinSceneTest.hpp"
#include "ReflTest.hpp"
// Asset thumbnail cache.
#include "ThumbnailCache.hpp"
#if AVER_MODULE_SCENE
#include "aver/render/SkinnedScene.hpp"
#endif
#include "aver/ui/ui_abi.h"

#include "ProjectBrowser.hpp"
#include "LevelClassSave.hpp"
#include "GraphClassPlacement.hpp"   // Place Class: the CLASS scan, placement record and marker
#include "LevelList.hpp"
#include "ProjectScaffold.hpp"
#include "GraphAssetPresentation.hpp"
#include "MaterialResolve.hpp"
#include "SurfaceName.hpp"
#include "ClusterMaterialShader.hpp"
// Scene submission routing. WARNING: aver::game::SurfaceLook here differs from nested SurfaceLook
// further down -- use qualified names inside class members.
#include "aver/game/SceneSubmission.hpp"
// Testable ray/triangle picking logic.
#include "ViewportPick.hpp"
// Play-side profiling.
#include "PlayProfile.hpp"

// Material sampler: fixed at s0, Voxi's samplers start at s1.
namespace { constexpr aver::u32 kClusterMaterialSamplerSlot = 0; }
#if AVER_MODULE_VOXI
// Voxi GI/shadow bindings (after cluster geometry SRVs).
namespace {
constexpr aver::u32 kClusterGiSrvBase       = 4;   // t4 GI volume, t5 shadow map
constexpr aver::u32 kClusterGiSamplerBase   = 1;   // s1 volume (linear-clamp), s2 shadow (comparison)
// b3 not b4 (AS/MS own b4 for ClusterFrameCB).
constexpr aver::u32 kClusterGiFrameRegister = 3;
}
#endif
#include "ToolsMenu.hpp"
#include "Nrd2Session.hpp"
#include "ShaderWarmup.hpp"
#include "UiRegistry.hpp"
#if AVER_MODULE_SYNAPSE
#include "NavBakeCommand.hpp"
#endif

#if AVER_MODULE_MCP
#include "aver/mcp/McpBridge.hpp"
#endif
#include "ToolGlyphs.hpp"
#include "EditorWidgets.hpp"
#include "EditorTheme.hpp"
#include "EditorChrome.hpp"
#include "FoliageAlign.hpp"
#include "LandscapePaintTool.hpp"   // wave3: the Landscape mode's Paint tab
#include "WaterSplineTool.hpp"      // wave3: the Landscape mode's Water tab
#include <functional>
#include "PlayerStartRefresh.hpp"
#include "LevelViewStore.hpp"
#include "AssetEditor.hpp"
#include "ActorEditor.hpp"
#include "AnimEditor.hpp"
#include "GraphEditor.hpp"
#include "BtEditor.hpp"
#include "BtGraphEditor.hpp"
#include "StateTreeEditor.hpp"   // AI: wave 3 #21
#include "EqsEditor.hpp"
#include "UiLayoutEditor.hpp"
#include "FontSetEditor.hpp"
#include "BlendSpaceEditor.hpp"
#include "AnimStateMachineEditor.hpp"
#include "MontageEditor.hpp"
#include "IkRigEditor.hpp"      // [wave5/ik-retarget]
#if AVER_MODULE_RAGDOLL
#include "PhysAnimEditor.hpp"   // .ocphysanim tab (W6 step 8)
#endif

#include "FaceRigEditor.hpp"    // [wave9/morph]
#include "RetargetEditor.hpp"
#include "RigEditor.hpp"
#include "MotionDbEditor.hpp"
#include "SoundEditor.hpp"
// Guarded: ParticleEditor.hpp includes OcParticle.hpp, which sandbox/CMakeLists.txt links only
// `if(TARGET Aver.Formats.Particles)` (gated on Aver.Particles). With AVER_MODULE_PARTICLES=OFF
// neither target exists, so every other reference to this tab (factory registration, shutdown,
// Content Browser create-menu) is guarded too.
#if AVER_MODULE_PARTICLES
#include "ParticleEditor.hpp"
#endif
// Unconditional, unlike ParticleEditor.hpp above: .ocfoliage is not gated behind any module -- see
// OcFoliage.hpp's own comment on why the format needs neither Aver.Scene nor Aver.Landscape.
#include "FoliageTypeEditor.hpp"
#include "OutlinerModel.hpp"      // Outliner type filter and folder paths
#include "ContentCollections.hpp"  // Content Browser chips, favourites, collections
#include "HairGroomTab.hpp"       // Groom tab (.ochair) and the Hair Details section
#include "HairDebugOverlay.hpp"   // Show > Hair
// Unconditional for the identical reason: OcInput.hpp's own comment states it needs no engine
// dependency either.
#include "InputSchemeEditor.hpp"
// Accessibility (docs/ACCESSIBILITY.md): the .occaptions editor tab, the colour-vision preview and the per-frame option/caption state.
#include "CaptionEditor.hpp"
#include "AccessibilityPreview.hpp"
#include "aver/game/GameAccessibility.hpp"
// Data tables (.csv/.json) and localization string tables: text formats in the base Aver.Formats target, so unconditional too.
#include "DataTableEditor.hpp"
#include "StringTableEditor.hpp"
#include "SoftBodyDebugOverlay.hpp"   // Show > Soft Bodies (also pulls the soft-body system header when the module is on)

#include "VehicleDebugOverlay.hpp"    // Show > Vehicles and the .ocvehicle editor's spec preview
#include "PhysicsDebugOverlay.hpp"    // Show > Physics (Jolt's debug renderer as a line feed) and the profiler's Physics (Jolt) section
#include "VehicleDetailsModel.hpp"    // VehicleSectionEnv, drawVehicleSection
#if AVER_MODULE_VEHICLE
#  include "VehicleEditor.hpp"        // makeVehicleEditor, vehicleStarterText
#endif
#include "RenderQueueOutput.hpp"
#include "RenderQueuePanel.hpp"
#include "PostVolumeTools.hpp"
#if AVER_MODULE_VOXI
#include "SceneLightFeed.hpp"
#include "DecalLevelIo.hpp"
#include "DecalGizmo.hpp"
#include "DecalAssets.hpp"
#include "PrefabEditorUi.hpp"
#include "AiDebugOverlay.hpp"
#if AVER_WITH_SYNAPSE_AI && AVER_MODULE_SYNAPSE_GPU
#include "aver/synapse/CrowdGpu.hpp"
#endif
#include "aver/game/SceneDecalFeed.hpp"
#include "aver/game/EnvFxFeed.hpp"   // G4 R8
#endif
#if AVER_MODULE_SCENE
#include "MotionDebugOverlay.hpp"   // motion matching overlay (wave 5 #25)

#include "CameraRigDebugOverlay.hpp"   // Show > Camera Rigs (wave 10, docs/CAMERAS.md)
// [wave3/level-instances begin] Level instances in the editor (docs/LEVEL_INSTANCES.md).
#include "LevelInstanceEditorUi.hpp"
#include "aver/game/LevelInstancesAbiHost.hpp"
#include "aver/game/LevelInstancesFramework.hpp"
#include "aver/game/LevelLights.hpp"
#include "aver/game/LevelComponents.hpp"   // the COMPONENT applier shared with the packaged game
#include "PlacedComponentModel.hpp"
#include "aver/game/LevelPackBaker.hpp"
// [wave3/level-instances end]
#endif
#include "EditorEuler.hpp"
#include "SequenceEditor.hpp"
#include "AssetRefScan.hpp"
#include "EditorTransform.hpp"   // dropRestLift, so a dropped asset rests on what it landed on
#include "EditorNotifications.hpp"
#include "EditorPrefs.hpp"
#include "EditorIcons.hpp"
#include "ViewportSnap.hpp"
#include "EditorKeybinds.hpp"
// The one place that decides who owns the keyboard and mouse this frame.
#include "ViewportIconRenderer.hpp"
#include "PhysicsSceneSync.hpp"
#include "InputOwnership.hpp"
#include "AverSrChoice.hpp"
#include "EditorConsole.hpp"
#include "EditorEntitySnapshot.hpp"
#include "EditorOriginParticipant.hpp"   // LARGE_WORLD: the editor's part of a floating-origin shift
#include "WorldConsole.hpp"              // world.* console commands (and the request they queue for slot S0)
#include "aver/platform/DirectoryWatcher.hpp"
#if AVER_HAVE_ROSLYN
#  include "aver/formats/AverDesign.hpp"
#endif
#include "EngineScaffold.hpp"
#include "McpConf.hpp"
#include "IdeIntegration.hpp"
#include "RuntimeLaunch.hpp"
#include "ShellIntegration.hpp"
// What git says about the open project. Pure header (see its own top comment, and tests/editor's
// RevisionControlTest, which compiles it with neither this file nor ImGui in sight); declares what
// porcelain v2's bytes mean and RevisionControl.cpp's four I/O entry points. ImGui and process-spawn
// code stays at the call sites below -- same split as InputOwnership.hpp / SceneSubmission.hpp.
#include "RevisionControl.hpp"

#if AVER_MODULE_VOXI
#include "aver/voxi/Voxi.hpp"
#include "aver/voxi/VoxiRenderer.hpp"
// Stage 3: the narrow GI/shadow HLSL slice the GPU per-cluster pipeline composes into
// ClusterMaterialShader.hpp's PSClusterMain. See its own header comment for what this is and is not.
#include "aver/voxi/VoxiGiShaders.hpp"
// Shared header-only two-phase manifest apply (settings-separation Lane 2); applyProjectVoxiSettings
// and captureRenderSettingsFromUi below are thin callers. Also pulls in QualityLadder.hpp,
// RenderSettingsResolver.hpp (voxi::resolve, used throughout buildRenderingSettings and the G-buffer
// switch) and Scalability.hpp (the Overall Quality preset) -- the only settings-separation include
// needed here.
#include "aver/voxi/ProjectRenderApply.hpp"
#include "aver/voxi/FrameBudget.hpp"
#endif

#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
#include "aver/occlusion/Occlusion.hpp"
#endif

#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
#include "aver/particles/ParticleEffectLibrary.hpp"
#include "aver/particles/ParticleRenderer.hpp"
#include "aver/particles/ParticleSystem.hpp"
#include "aver/formats/OcParticle.hpp"
#endif

// Fluids and Soft-Body are independent of Particles.
#if AVER_MODULE_RENDER_SOFTBODY && AVER_MODULE_SCENE
#include "aver/render/SoftBodyScene.hpp"
#endif
#if AVER_MODULE_RENDER_HAIR && AVER_MODULE_SCENE
#include "aver/render/HairScene.hpp"
#endif

#if AVER_MODULE_PBR
#include "aver/pbr/Material.hpp"
#include "aver/pbr/MaterialGpu.hpp"
#include "aver/pbr/MaterialGraphRegistry.hpp"
#include "aver/pbr/MaterialSystem.hpp"
#include "aver/pbr/PbrShaders.hpp"
#include "aver/formats/OcMat.hpp"
#include "aver/formats/MaterialScript.hpp"
#include "aver/assets/TextureUpload.hpp"
#endif

// Sky/atmosphere mapping: needed even when PBR=OFF.
#include "aver/assets/LevelSky.hpp"

#if AVER_MODULE_SCRIPTING
#include "aver/scripting/ScriptHost.hpp"
#endif

// Spatial upscaler composition root.
#if AVER_MODULE_SR
#include "aver/sr/AverSrQuality.hpp"
#include "aver/sr/AverSrFsr.hpp"
#include "aver/sr/AverSrTaa.hpp"
#include "aver/sr/NeuRaa.hpp"
// AverSR level constants must match Voxi ladder numbering.
#if AVER_MODULE_VOXI
static_assert(static_cast<aver::u32>(aver::sr::Quality::Off)         == aver::voxi::ladder::kAverSrOff,
             "aver::sr::Quality::Off no longer matches aver::voxi::ladder::kAverSrOff");
static_assert(static_cast<aver::u32>(aver::sr::Quality::Quality)     == aver::voxi::ladder::kAverSrQuality,
             "aver::sr::Quality::Quality no longer matches aver::voxi::ladder::kAverSrQuality");
static_assert(static_cast<aver::u32>(aver::sr::Quality::Balanced)    == aver::voxi::ladder::kAverSrBalanced,
             "aver::sr::Quality::Balanced no longer matches aver::voxi::ladder::kAverSrBalanced");
static_assert(static_cast<aver::u32>(aver::sr::Quality::Performance) == aver::voxi::ladder::kAverSrPerformance,
             "aver::sr::Quality::Performance no longer matches aver::voxi::ladder::kAverSrPerformance");
#endif  // AVER_MODULE_VOXI
#endif  // AVER_MODULE_SR

// Frame interpolation.
#include "aver/neurafi/NeuraFI.hpp"

// Guarded on PHYSICS alone, not FRAMEWORK (SCENE=OFF can decouple them).
#if AVER_MODULE_PHYSICS
#include "aver/physics/physics_abi.h"
#endif
#if AVER_MODULE_FRAMEWORK
#include "aver/framework/framework_abi.h"
#include "aver/framework/framework_state_abi.h"
#include "aver/framework/framework_hooks.h"
#endif

// Win32 headers (applyMcpCommand has no scene dependency).
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#if AVER_MODULE_SCENE
#include "aver/scene/scene_abi.h"
#include "aver/anim/AnimSystem.hpp"
#include "aver/anim/ControlRig.hpp"
#include "aver/save/SaveWorld.hpp"
#include "aver/formats/OcSave.hpp"
#include "aver/scene/World.hpp"
#include "aver/scene/Components.hpp"
// Shared placement -> entity loop.
#include "aver/world/LevelInstance.hpp"
// Scene load divergence census.
#include "aver/world/SceneCensus.hpp"
// Scatter generation palette.
#include "aver/world/ScatterPalette.hpp"
#endif
#if AVER_MODULE_SYNAPSE_SCENE
#include "aver/synapse/SynapseAgent.hpp"
#include "aver/synapse/SynapsePerception.hpp"
#include "aver/synapse/SynapseBt.hpp"
#endif

#if AVER_WITH_IMGUI
#include "imgui.h"
#include "imgui_internal.h"
#endif
#if AVER_WITH_IMGUI_D3D12
// ImGui D3D12 backend.
#include "aver/rhi/d3d12/UiBackend.hpp"
#include "aver/rhi/d3d12/ImGuiUiBackend.hpp"
#endif
#if AVER_WITH_IMGUI_VULKAN
// ImGui Vulkan backend.
#include "aver/rhi/vulkan/UiBackend.hpp"
#include "aver/rhi/vulkan/ImGuiUiBackend.hpp"
#endif

#include "stb_image_write.h"

#include <algorithm>
#include <atomic>       // the revision-control workers' done flag; not left to ToolsMenu.hpp's include
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <new>          // ::operator new, for --crash-test oom
#include <optional>
#include <stdexcept>    // std::runtime_error, for --crash-test throw
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aver {

// Window event sink callback.
static inline void sandboxWindowEvent(void* user, const Event& e) {
    static_cast<InputState*>(user)->onEvent(e);
}

#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
namespace {

// Framework seam: spawns preview (no BeginPlay), allows patching, then dispatches BeginPlay.
inline aver::scene::Entity saveSpawnClass(const char* className, void*) {
    const i32 c = aver_fw_class_find(className);
    if (c == 0) return aver::scene::kInvalidEntity;
    const i32 e = aver_fw_spawn_preview(c, className, nullptr, nullptr, nullptr);
    return e == 0 ? aver::scene::kInvalidEntity : static_cast<aver::scene::Entity>(e);
}

inline const char* saveClassOf(aver::scene::Entity e, void*) {
    const i32 c = aver_fw_class_of(static_cast<i32>(e));
    return c == 0 ? nullptr : aver_fw_class_name(c);
}

inline void saveBeginPlay(aver::scene::Entity e, void*) {
    aver_fw_dispatch_begin_play(static_cast<i32>(e), AVER_FW_BEGIN_SPAWN);
}

// Through the framework, so OnEndPlay runs and the managed instance is released. World::destroy
// would take the entity out from under a live C# object.
inline void saveDestroyActor(aver::scene::Entity e, void*) { aver_fw_destroy(static_cast<i32>(e)); }

// Graph local variables relay.
inline i32 saveGraphVarCount(aver::scene::Entity e, void*) {
    return aver_fw_graph_var_count(static_cast<i32>(e));
}
inline i32 saveGraphVarAt(aver::scene::Entity e, i32 index, char* nameBuf, i32 nameBufLen,
                   u32* outKind, f32* outF, i32* outI, void*) {
    return aver_fw_graph_var_at(static_cast<i32>(e), index, nameBuf, nameBufLen,
                                reinterpret_cast<int32_t*>(outKind), outF, outI);
}
inline i32 saveGraphVarSet(aver::scene::Entity e, const char* name, u32 kind, f32 f, i32 i, void*) {
    return aver_fw_graph_var_set(static_cast<i32>(e), name, static_cast<int32_t>(kind), f, i);
}

// Managed entity state (framework_state_abi.h) relay.
inline i32 saveStateCount(aver::scene::Entity e, void*) {
    return std::max(0, static_cast<i32>(aver_fw_state_count(static_cast<i32>(e)))) + aver::game::softBodyStateCount(e);
}
inline i32 saveStateAt(aver::scene::Entity e, i32 index, char* nameBuf, i32 nameCap, char* dataBuf,
                       i32 dataCap, void*) {
    // The managed providers first, then the soft-body damage entry (docs/SOFTBODY.md, saved damage).
    const i32 managed = std::max(0, static_cast<i32>(aver_fw_state_count(static_cast<i32>(e))));
    if (index < managed) return aver_fw_state_at(static_cast<i32>(e), index, nameBuf, nameCap, dataBuf, dataCap);
    return aver::game::softBodyStateAt(e, index - managed, nameBuf, nameCap, dataBuf, dataCap);
}
inline i32 saveStateSet(aver::scene::Entity e, const char* name, const char* data, i32 dataLen, void*) {
    if (aver_fw_state_set(static_cast<i32>(e), name, data, dataLen)) return 1;
    return aver::game::softBodyStateSet(e, name, data, dataLen);
}

inline aver::save::Host saveHost() {
    aver::save::Host h;
    h.spawnClass    = &saveSpawnClass;
    h.classOf       = &saveClassOf;
    h.beginPlay     = &saveBeginPlay;
    h.destroyActor  = &saveDestroyActor;
    h.graphVarCount = &saveGraphVarCount;
    h.graphVarAt    = &saveGraphVarAt;
    h.graphVarSet   = &saveGraphVarSet;
    h.stateCount    = &saveStateCount;
    h.stateAt       = &saveStateAt;
    h.stateSet      = &saveStateSet;
    return h;
}

// Not saved: transient soft-body field entities, and whatever a streamer made (PCG scatter, a cooked level's chunks), which has its
// own persistence and would otherwise be restored as a second, permanent copy. `user` is the editor's OriginParticipants.
inline bool saveSkip(aver::scene::Entity e, void* user) {
    return aver::game::softBodySaveSkip(e, nullptr) || aver::game::OriginParticipants::skipStreamed(e, user);
}

inline i32 saveWriteProvider(const char* path, void* user) {
    auto* origin = static_cast<aver::game::OriginParticipants*>(user);
    aver::save::CaptureOptions co;
    co.host = saveHost();
    co.host.user = user;
    co.skip = &saveSkip;
    if (origin) { co.originX = origin->originX(); co.originY = origin->originY(); }   // the ORIG chunk
    aver::fmt::OcSaveData snap;
    std::string why;
    if (!aver::save::capture(aver::scene::World::instance(), snap, co, &why)) {
        AVER_ERROR("[Save] capture failed: {}", why);
        return 0;
    }
    if (!aver::fmt::saveOcSave(path, snap, &why)) {
        AVER_ERROR("[Save] write failed: {}", why);
        return 0;
    }
    AVER_INFO("[Save] wrote {} ({} entities)", path, snap.entities.size());
    return 1;
}

inline i32 saveLoadProvider(const char* path, void* user) {
    auto* origin = static_cast<aver::game::OriginParticipants*>(user);
    aver::fmt::OcSaveData snap;
    std::string why;
    if (!aver::fmt::loadOcSave(path, snap, &why)) {
        AVER_ERROR("[Save] load failed: {}", why);
        return 0;
    }
    aver::save::RestoreOptions ro;
    ro.host = saveHost();
    ro.host.user = user;
    ro.skip = &aver::game::OriginParticipants::skipStreamed;   // the teardown leaves resident chunks alone, as capture left them out
    if (origin) { ro.originX = origin->originX(); ro.originY = origin->originY(); }
    aver::game::softBodyResetForRestore();   // the restore pass replays saved damage onto rebuilt bodies
    if (!aver::save::restore(snap, aver::scene::World::instance(), ro, &why)) {
        AVER_ERROR("[Save] restore failed: {}", why);
        return 0;
    }
    return 1;
}

} // namespace
#endif


// Gizmo colours: X red, Y green, Z blue, yellow highlight (UE5 palette).
static const Vec3 kAxisDir[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
static const Vec3 kAxisCol[3] = {{202.0f/255.0f, 38.0f/255.0f, 0.0f}, {103.0f/255.0f, 169.0f/255.0f, 0.0f},
                                  {44.0f/255.0f, 126.0f/255.0f, 237.0f/255.0f}};
static const Vec3 kAxisHi = {1.0f, 1.0f, 0.0f};

using rhi::primitives::appendBox;
using rhi::primitives::appendSphere;
// Ground quad in XY plane with half-extent s.
static inline void appendGround(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx, f32 s) {
    const u32 b = static_cast<u32>(v.size());
    v.push_back({-s,-s,0,0,0,1,-0.5f,-0.5f}); v.push_back({s,-s,0,0,0,1,0.5f,-0.5f});
    v.push_back({s,s,0,0,0,1,0.5f,0.5f}); v.push_back({-s,s,0,0,0,1,-0.5f,0.5f});
    idx.push_back(b); idx.push_back(b+1); idx.push_back(b+2); idx.push_back(b); idx.push_back(b+2); idx.push_back(b+3);
}

using rhi::primitives::appendBoxYaw;
using rhi::primitives::appendCylinderZ;


// Placeholder quadcopter from appendBoxYaw/appendCylinderZ.
static inline void appendDrone(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx) {
    // Central body: squarish, flatter than wide.
    constexpr f32 kBodyHX = 0.26f, kBodyHY = 0.26f, kBodyHZ = 0.15f;
    appendBoxYaw(v, idx, 0, 0, 0, kBodyHX, kBodyHY, kBodyHZ, 0.0f);

    // Four arms with motor pods and rotors.
    constexpr f32 kArmAngleDeg[4] = {45.0f, 135.0f, 225.0f, 315.0f};
    constexpr f32 kArmRInner = 0.34f;  // just inside the body's own corner (0.26*sqrt2 = 0.368) -- no seam
    constexpr f32 kArmROuter = 0.80f;  // hub distance from the drone's centre
    constexpr f32 kArmHalfLen = (kArmROuter - kArmRInner) * 0.5f;
    constexpr f32 kArmCenterR = (kArmROuter + kArmRInner) * 0.5f;
    constexpr f32 kArmHalfWidth = 0.045f, kArmHalfThick = 0.032f;
    constexpr f32 kHubRadius = 0.11f, kHubHalfHeight = 0.05f, kHubCenterZ = 0.08f;
    constexpr f32 kDiscRadius = 0.20f, kDiscHalfHeight = 0.014f, kDiscCenterZ = 0.14f;
    for (f32 deg : kArmAngleDeg) {
        const f32 rad = deg * kDegToRad;
        const f32 armX = kArmCenterR * std::cos(rad), armY = kArmCenterR * std::sin(rad);
        appendBoxYaw(v, idx, armX, armY, 0.0f, kArmHalfLen, kArmHalfWidth, kArmHalfThick, deg);

        const f32 hubX = kArmROuter * std::cos(rad), hubY = kArmROuter * std::sin(rad);
        appendCylinderZ(v, idx, hubX, hubY, kHubCenterZ, kHubRadius, kHubHalfHeight, 8);
        // Static rotor disc placeholder.
        appendCylinderZ(v, idx, hubX, hubY, kDiscCenterZ, kDiscRadius, kDiscHalfHeight, 10);
    }

    // Landing skids and support struts.
    constexpr f32 kSkidHalfLen = 0.30f, kSkidHalfWidth = 0.02f, kSkidHalfThick = 0.018f;
    constexpr f32 kSkidY = 0.20f, kSkidZ = -0.20f;
    appendBoxYaw(v, idx, 0.0f,  kSkidY, kSkidZ, kSkidHalfLen, kSkidHalfWidth, kSkidHalfThick, 0.0f);
    appendBoxYaw(v, idx, 0.0f, -kSkidY, kSkidZ, kSkidHalfLen, kSkidHalfWidth, kSkidHalfThick, 0.0f);

    constexpr f32 kStrutHalfX = 0.02f, kStrutHalfY = 0.02f, kStrutHalfZ = 0.016f;
    // Midpoint between body and skid.
    constexpr f32 kStrutX = 0.16f, kStrutZ = -0.166f;
    for (f32 sx : {-kStrutX, kStrutX})
        for (f32 sy : {-kSkidY, kSkidY})
            appendBoxYaw(v, idx, sx, sy, kStrutZ, kStrutHalfX, kStrutHalfY, kStrutHalfZ, 0.0f);
}

// Euler<->quaternion conversion (in EditorEuler.hpp).
using aver::editor::quatFromEulerDeg;
using aver::editor::eulerDegFromQuat;

// Snap to grid (step).
static inline f32 snapf(f32 v, f32 step) { return editor::snapValue(v, step); }

// Transform point (row-vector matrix).
static inline Vec3 xformPoint(const Mat4& m, const Vec3& p) {
    return { p.x*m.m[0][0]+p.y*m.m[1][0]+p.z*m.m[2][0]+m.m[3][0],
             p.x*m.m[0][1]+p.y*m.m[1][1]+p.z*m.m[2][1]+m.m[3][1],
             p.x*m.m[0][2]+p.y*m.m[1][2]+p.z*m.m[2][2]+m.m[3][2] };
}
// Transform direction (no translation).
static inline Vec3 xformVec(const Mat4& m, const Vec3& v) {
    return { v.x*m.m[0][0]+v.y*m.m[1][0]+v.z*m.m[2][0],
             v.x*m.m[0][1]+v.y*m.m[1][1]+v.z*m.m[2][1],
             v.x*m.m[0][2]+v.y*m.m[1][2]+v.z*m.m[2][2] };
}
// Ray-AABB intersection, entry distance to tHit.
static inline bool rayAabb(const Vec3& o, const Vec3& d, const Vec3& mn, const Vec3& mx, f32& tHit) {
    f32 tmin = 0.0f, tmax = 1e30f;
    for (int a = 0; a < 3; ++a) {
        const f32 od = (&o.x)[a], dd = (&d.x)[a], lo = (&mn.x)[a], hi = (&mx.x)[a];
        if (std::fabs(dd) < 1e-8f) { if (od < lo || od > hi) return false; }
        else {
            f32 t1 = (lo - od) / dd, t2 = (hi - od) / dd;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::fmax(tmin, t1); tmax = std::fmin(tmax, t2);
            if (tmin > tmax) return false;
        }
    }
    tHit = tmin; return true;
}
// MeshVertex to PickGeometry (built-in meshes only).
static inline aver::editor::PickGeometry buildPickGeometry(const std::vector<rhi::MeshVertex>& v,
                                                      const std::vector<u32>& idx) {
    aver::editor::PickGeometry g;
    g.positions.reserve(v.size() * 3);
    g.normals.reserve(v.size() * 3);
    for (const rhi::MeshVertex& mv : v) {
        g.positions.push_back(mv.px); g.positions.push_back(mv.py); g.positions.push_back(mv.pz);
        g.normals.push_back(mv.nx);   g.normals.push_back(mv.ny);   g.normals.push_back(mv.nz);
    }
    g.indices = idx;
    return g;
}
// Floor grid with axis markers.
static inline void buildGrid(std::vector<rhi::LineVertex>& v, f32 ext, f32 step) {
    const f32 g = 0.26f;
    const f32 lift = step * 0.02f;
    for (f32 x = -ext; x <= ext + step * 0.001f; x += step) {
        v.push_back({x, -ext, lift, g, g, g}); v.push_back({x, ext, lift, g, g, g});
    }
    for (f32 y = -ext; y <= ext + step * 0.001f; y += step) {
        v.push_back({-ext, y, lift, g, g, g}); v.push_back({ext, y, lift, g, g, g});
    }
    const f32 axisLift = lift * 1.5f;
    v.push_back({0,0,axisLift, 0.80f,0.25f,0.25f}); v.push_back({ext,0,axisLift, 0.80f,0.25f,0.25f});
    v.push_back({0,0,axisLift, 0.28f,0.72f,0.30f}); v.push_back({0,ext,axisLift, 0.28f,0.72f,0.30f});
}

// Add coloured line segment.
static inline void gzLine(std::vector<rhi::LineVertex>& v, const Vec3& a, const Vec3& b, const Vec3& c) {
    v.push_back({a.x,a.y,a.z, c.x,c.y,c.z}); v.push_back({b.x,b.y,b.z, c.x,c.y,c.z});
}
// Translate arrow for axis a.
static inline std::vector<rhi::LineVertex> buildMoveAxis(int a, const Vec3& c) {
    std::vector<rhi::LineVertex> v;
    const Vec3 A = kAxisDir[a], P = kAxisDir[(a+1)%3], Q = kAxisDir[(a+2)%3];
    gzLine(v, {0,0,0}, A, c);
    const Vec3 tip = A, base = A * 0.80f;
    for (int k = 0; k < 4; ++k) { f32 t = k * (kPi * 0.5f); Vec3 r = P*(std::cos(t)*0.07f) + Q*(std::sin(t)*0.07f); gzLine(v, base+r, tip, c); }
    return v;
}
// Rotation ring perpendicular to axis a.
static inline std::vector<rhi::LineVertex> buildRotRing(int a, const Vec3& c) {
    std::vector<rhi::LineVertex> v;
    const Vec3 P = kAxisDir[(a+1)%3], Q = kAxisDir[(a+2)%3];
    const int N = 64; Vec3 prev{};
    for (int k = 0; k <= N; ++k) { f32 t = k * (kTwoPi / N); Vec3 p = P*std::cos(t) + Q*std::sin(t); if (k > 0) gzLine(v, prev, p, c); prev = p; }
    return v;
}
// Scale handle for axis a (shaft with box tip).
static inline std::vector<rhi::LineVertex> buildScaleAxis(int a, const Vec3& c) {
    std::vector<rhi::LineVertex> v;
    const Vec3 A = kAxisDir[a], P = kAxisDir[(a+1)%3], Q = kAxisDir[(a+2)%3];
    gzLine(v, {0,0,0}, A * 0.86f, c);
    const Vec3 ctr = A * 0.93f; const f32 h = 0.07f;
    Vec3 cor[8]; int i = 0;
    for (int sx = -1; sx <= 1; sx += 2) for (int sy = -1; sy <= 1; sy += 2) for (int sz = -1; sz <= 1; sz += 2)
        cor[i++] = ctr + A*(h*sx) + P*(h*sy) + Q*(h*sz);
    const int e[12][2] = {{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
    for (auto& pr : e) gzLine(v, cor[pr[0]], cor[pr[1]], c);
    return v;
}

// Player Start capsule wire (UE style: rings, sides, pole arcs).
static inline std::vector<rhi::LineVertex> buildCapsuleWire(f32 radius, f32 halfHeight, const Vec3& c) {
    std::vector<rhi::LineVertex> v;
    const f32 tall = halfHeight * 2.0f;              // pole-to-pole height
    const f32 zLo = radius, zHi = tall - radius;      // hemisphere seam heights
    constexpr int kSeg = 24;
    for (const f32 z : {zLo, zHi}) {
        Vec3 prev{};
        for (int k = 0; k <= kSeg; ++k) {
            const f32 t = k * (kTwoPi / kSeg);
            const Vec3 p{radius * std::cos(t), radius * std::sin(t), z};
            if (k > 0) gzLine(v, prev, p, c);
            prev = p;
        }
    }
    for (int k = 0; k < 4; ++k) {
        const f32 t = k * (kPi * 0.5f);
        const f32 sx = radius * std::cos(t), sy = radius * std::sin(t);
        gzLine(v, {sx, sy, zLo}, {sx, sy, zHi}, c);
    }
    // Half circles: 180-degree arcs through each dome.
    constexpr int kHalfSeg = kSeg / 2;
    const f32 seamZ[2] = {zLo, zHi};
    const f32 sign[2]  = {-1.0f, 1.0f};   // bottom dome curves toward z=0, top toward z=tall
    for (int cap = 0; cap < 2; ++cap) {
        for (int plane = 0; plane < 2; ++plane) {   // 0 = XZ, 1 = YZ
            Vec3 prev{};
            for (int k = 0; k <= kHalfSeg; ++k) {
                const f32 t = k * (kPi / kHalfSeg);
                const f32 rx = radius * std::cos(t);
                const f32 rz = seamZ[cap] + sign[cap] * radius * std::sin(t);
                const Vec3 p = plane == 0 ? Vec3{rx, 0.0f, rz} : Vec3{0.0f, rx, rz};
                if (k > 0) gzLine(v, prev, p, c);
                prev = p;
            }
        }
    }
    return v;
}
// Player Start facing arrow (local +X).
static inline std::vector<rhi::LineVertex> buildPlayerStartArrow(f32 centerZ, f32 length, const Vec3& c) {
    std::vector<rhi::LineVertex> v;
    const Vec3 base{0.0f, 0.0f, centerZ}, tip{length, 0.0f, centerZ};
    gzLine(v, base, tip, c);
    const f32 headLen = length * 0.28f, headWidth = length * 0.16f;
    const Vec3 back{tip.x - headLen, 0.0f, centerZ};
    gzLine(v, tip, {back.x, headWidth, centerZ}, c);
    gzLine(v, tip, {back.x, -headWidth, centerZ}, c);
    return v;
}

// Editor modes: Select (objects), Landscape (terrain), Foliage (scatter), Simulate (play), Animate (level sequence).
enum class EditorMode { Select, Landscape, Foliage, Simulate, Animate, Modeling };
static const char* kEditorModeNames[6] = {"Select", "Landscape", "Foliage", "Simulate", "Animate", "Modeling"};
// Mode descriptions.
static const char* kEditorModeHints[6] = {
    "Pick and transform objects",
    "Sculpt the terrain heightfield",
    "Paint scattered meshes onto the terrain",
    "Run the game in the viewport",
    "Keyframe actors, a camera path and emissive glow on one level timeline",
    "Build and edit a mesh: primitives, vertex / edge / face selection, extrude, inset, bevel, booleans, UVs",
};
static constexpr int kEditorModeCount = 6;

// Object tools. Only meaningful in EditorMode::Select.
enum class Tool { Select, Move, Rotate, Scale };
static const char* kToolNames[4] = {"Select", "Move", "Rotate", "Scale"};

#if AVER_MODULE_LANDSCAPE
// Terrain brushes. Only meaningful in EditorMode::Landscape.
enum class SculptTool { Raise, Lower, Smooth, Flatten, Ramp, Noise };
static const char* kSculptToolNames[6] = {"Raise", "Lower", "Smooth", "Flatten", "Ramp", "Noise"};
#endif

// Which bottom drawer is up. Only one at a time.
enum class Drawer { None, Content, Log, Console };

#if AVER_WITH_IMGUI
// Fixed editor chrome (toolbar / status bar / dock host): no decoration, never steals focus.
static constexpr ImGuiWindowFlags kChromeFlags =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

// Bottom drawer window: like the chrome, but may come to the front, and never scrolls.
static constexpr ImGuiWindowFlags kDrawerFlags =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings;
#endif

// The editor's placeholder scene dimensions, in centimetres.
inline constexpr f32 kEditorFloorHalf = 1000.0f;   // cm
inline constexpr f32 kEditorCubeHalf  = 50.0f;     // cm
// Player Start marker, like Unreal's APlayerStart: a wire capsule standing on the spawn point (the
// pawn's FEET -- the marker's own origin), a facing arrow, and a billboard sprite at the capsule's
// centre. Capsule/arrow colours are Unreal's own, sRGB; the sprite half-size is world units, same
// convention the old pin icon used.
inline constexpr f32 kPlayerStartCapsuleRadius = 40.0f;       // cm
inline constexpr f32 kPlayerStartCapsuleHalfHeight = 92.0f;   // cm (184 cm tall, pole to pole)
inline constexpr f32 kPlayerStartArrowLength = 80.0f;         // cm
inline constexpr f32 kPlayerStartIconHalfSize = 16.0f;        // cm
static const Vec3 kPlayerStartColor = {1.0f, 138.0f/255.0f, 5.0f/255.0f};                  // sRGB (255,138,5)
static const Vec3 kPlayerStartArrowColor = {150.0f/255.0f, 200.0f/255.0f, 1.0f};           // sRGB (150,200,255)

// Unreal's selection colour, sRGB (235,163,10). File-scope (not local to selectionOutlineLines,
// which used to be its only user) since the Player Start's capsule needs the same colour when it is
// itself the selected actor.
static const Vec3 kSelectionColor = {235.0f/255.0f, 163.0f/255.0f, 10.0f/255.0f};
inline constexpr f32 kEditorGridCell  = 100.0f;    // cm
inline constexpr f32 kEditorGridHalf  = 1000.0f;   // cm
// How far in front of the camera Add places a new object.
inline constexpr f32 kAddDistance     = 400.0f;    // cm
// Duplicate's fallback nudge off the original when move-snap is off (snapped, it uses moveSnap_ so
// the copy lands on the same grid as the original).
inline constexpr f32 kDuplicateOffset = 50.0f;     // cm

// Drag-drop payload carrying a content-browser item's full path as bytes (content browser ->
// viewport asset placement). Under ImGui's 32-char payload-type limit; not prefixed with '_'.
static constexpr const char* kAssetDragDropType = "AVER_ASSET_PATH";
// A Place Actors palette entry dragged into the viewport: the NUL-terminated PlaceItem::kind.
static constexpr const char* kPlaceActorDragDropType = "AVER_PLACE_ACTOR";
// World Outliner's reparent drag. Fixed 4-byte scene::Entity, so every target guards on size
// EQUALITY (kAssetDragDropType's payload is a variable-length path and cannot).
static constexpr const char* kOutlinerReparentDragDropType = "AVER_OUTLINER_ENTITY";

// Dragging assets BETWEEN Content Browser folders (not into the level). A SECOND type, not a reuse
// of kAssetDragDropType: that one is filtered to .ocmesh/.ocparticle for the viewport, but moving
// files has no such restriction -- reusing it would silently drop unsupported types from the move.
// Both payloads are set from the same drag (ImGui allows several types per source); each target
// reads only the type it understands.
static constexpr const char* kCbMoveDragDropType = "AVER_CB_MOVE_SET";

// Placed object with mesh, transform, surface parameters.
struct MeshObj {
    std::string name;
    rhi::MeshHandle mesh = 0;
    u32 tris = 0;
    Vec3 pos{0,0,0}, rotDeg{0,0,0}, scale{1,1,1};
    f32 color[4] = {0.8f,0.4f,0.25f,1};
    f32 metallic = 0.0f, roughness = 0.5f;
#if AVER_MODULE_PBR
    pbr::MaterialHandle material = 0;
#endif
    bool visible = true;
    Vec3 aabbMin{-1,-1,-1}, aabbMax{1,1,1}; // local space
};

#if AVER_WITH_IMGUI
// Aver orange and dim variant.
static constexpr ImVec4 kAverOrange   (242.0f/255.0f, 101.0f/255.0f, 34.0f/255.0f, 1.00f);
static constexpr ImVec4 kAverOrangeDim(242.0f/255.0f, 101.0f/255.0f, 34.0f/255.0f, 0.55f);

// Editor dark scheme colours and metrics (split from applyDpi for DPI scaling order). The values come
// from editor::editorTheme() -- EditorTheme.hpp's defaults, overridden by editor-theme.json.
static inline ImVec4 toVec4(const editor::Rgba& c) { return ImVec4(c.r, c.g, c.b, c.a); }

static inline void applyEditorMetrics() {
    ImGuiStyle& s = ImGui::GetStyle();
    const editor::EditorTheme& t = editor::editorTheme();

    // Unscaled; applyDpi applies DPI scaling after.
    s.WindowRounding    = t.rounding;  s.ChildRounding  = t.rounding;  s.FrameRounding  = t.rounding;
    s.PopupRounding     = t.rounding;  s.GrabRounding   = t.grabRounding; s.TabRounding = t.tabRounding;
    s.ScrollbarRounding = t.rounding;
    s.WindowBorderSize  = 1;  s.FrameBorderSize = 0; s.PopupBorderSize = 1;
    s.ChildBorderSize   = 1;
    s.WindowPadding     = ImVec2(t.windowPaddingX, t.windowPaddingY);
    // FramePadding.y stays at 4 -- gates constraint: docked tab bar height = FontSize + FramePadding.y*2,
    // and --probe-rel (vpX_/vpY_/vpW_/vpH_) survives a resize but not an aspect change. The x half is free.
    // It is deliberately not a theme entry.
    s.FramePadding      = ImVec2(t.framePaddingX, 4);
    s.ItemSpacing       = ImVec2(t.itemSpacingX, t.itemSpacingY);
    s.ItemInnerSpacing  = ImVec2(6, 4);
    s.CellPadding       = ImVec2(6, 4);
    s.ScrollbarSize     = t.scrollbarSize;
    s.GrabMinSize       = 10;
    s.IndentSpacing     = t.indentSpacing;
    s.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
    s.SeparatorTextBorderSize = 1;
    // One close button per tab: the dock node's own X (it closes the whole node) and the window-menu
    // arrow (a collapse triangle that does nothing useful on a docked panel) are both off.
    s.DockingNodeHasCloseButton = false;
    s.WindowMenuButtonPosition  = ImGuiDir_None;
    // A wide splitter gap with a 1px line drawn in it (editor::drawDockSplitterLines); the gap is the hit area.
    s.DockingSeparatorSize = t.splitterSize;
    // A 1px rule under the tab bar rather than ImGui's default; the selected tab's overline stays.
    s.TabBarBorderSize = 1;
    s.TabBarOverlineSize = 2;
}

// Editor colour palette (safe to re-run outside widget draw). Orange is for selection, checkmarks, the
// active tab's top edge and Play; hover and press are neutral lifts of the item colour.
static inline void applyEditorColors() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;
    const editor::EditorTheme& th = editor::editorTheme();

    const ImVec4 sunken  = toVec4(th.sunken);      // behind content: viewports, child frames
    const ImVec4 panel   = toVec4(th.panel);       // window bodies
    const ImVec4 raised  = toVec4(th.raised);      // title bars, menu bar, inactive tabs
    const ImVec4 item    = toVec4(th.item);        // buttons, frames, headers at rest
    const ImVec4 itemHot = toVec4(editor::itemHot(th));
    const ImVec4 itemOn  = toVec4(editor::itemOn(th));
    const ImVec4 line    = toVec4(th.line);        // borders and separators
    const ImVec4 text    = toVec4(th.text);
    const ImVec4 textDim = toVec4(th.textDim);
    const ImVec4 accent  = toVec4(th.accent);
    const ImVec4 select  = toVec4(editor::selection(th));
    const ImVec4 accentDim = toVec4(editor::withAlpha(th.accent, 0.55f));
    const ImVec4 gripHot = toVec4(editor::mixToWhite(th.item, th.activeLift * 2.0f));   // scrollbar grab hovered, a step above itemOn

    c[ImGuiCol_Text]                  = text;
    c[ImGuiCol_TextDisabled]          = textDim;
    c[ImGuiCol_WindowBg]              = panel;
    c[ImGuiCol_ChildBg]               = sunken;
    c[ImGuiCol_PopupBg]               = toVec4(th.popup);
    c[ImGuiCol_Border]                = line;
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_FrameBg]               = item;
    c[ImGuiCol_FrameBgHovered]        = itemHot;
    c[ImGuiCol_FrameBgActive]         = itemOn;

    c[ImGuiCol_TitleBg]               = raised;
    c[ImGuiCol_TitleBgActive]         = raised;
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(sunken.x, sunken.y, sunken.z, 0.85f);
    c[ImGuiCol_MenuBarBg]             = raised;

    c[ImGuiCol_ScrollbarBg]           = ImVec4(sunken.x, sunken.y, sunken.z, 0.60f);
    c[ImGuiCol_ScrollbarGrab]         = itemHot;
    c[ImGuiCol_ScrollbarGrabHovered]  = gripHot;
    c[ImGuiCol_ScrollbarGrabActive]   = textDim;

    c[ImGuiCol_CheckMark]             = accent;
    c[ImGuiCol_SliderGrab]            = textDim;
    c[ImGuiCol_SliderGrabActive]      = text;

    c[ImGuiCol_Button]                = item;
    c[ImGuiCol_ButtonHovered]         = itemHot;
    c[ImGuiCol_ButtonActive]          = itemOn;

    // Header is the SELECTED row for Selectable and TreeNode: orange at selectionAlpha. Hover is a
    // translucent white lift, so it reads as neutral over any row. CollapsingHeader also paints Header
    // at rest, which is why every section header goes through editor::sectionHeader (EditorChrome.hpp),
    // which keeps them neutral.
    c[ImGuiCol_Header]                = select;
    c[ImGuiCol_HeaderHovered]         = ImVec4(1, 1, 1, th.hoverLift + 0.01f);
    c[ImGuiCol_HeaderActive]          = toVec4(editor::withAlpha(th.accent, th.selectionAlpha + 0.15f));

    c[ImGuiCol_Separator]             = line;
    c[ImGuiCol_SeparatorHovered]      = textDim;
    c[ImGuiCol_SeparatorActive]       = text;

    c[ImGuiCol_ResizeGrip]            = ImVec4(itemHot.x, itemHot.y, itemHot.z, 0.60f);
    c[ImGuiCol_ResizeGripHovered]     = textDim;
    c[ImGuiCol_ResizeGripActive]      = text;

    // Orange top edge on a panel-coloured body reads as "this one" without shouting (a fully orange
    // tab would).
    c[ImGuiCol_Tab]                   = raised;
    c[ImGuiCol_TabHovered]            = itemHot;
    c[ImGuiCol_TabActive]             = panel;
    c[ImGuiCol_TabSelectedOverline]   = accent;
    c[ImGuiCol_TabUnfocused]          = sunken;
    c[ImGuiCol_TabUnfocusedActive]    = raised;
    c[ImGuiCol_TabDimmedSelectedOverline] = accentDim;

    c[ImGuiCol_DockingPreview]        = select;
    c[ImGuiCol_DockingEmptyBg]        = sunken;

    c[ImGuiCol_PlotLines]             = textDim;
    c[ImGuiCol_PlotLinesHovered]      = text;
    c[ImGuiCol_PlotHistogram]         = accent;      // progress bars: the one non-selection use, kept so progress reads at a glance
    c[ImGuiCol_PlotHistogramHovered]  = ImVec4(1.000f, 0.478f, 0.208f, 1.00f);

    c[ImGuiCol_TableHeaderBg]         = raised;
    c[ImGuiCol_TableBorderStrong]     = line;
    c[ImGuiCol_TableBorderLight]      = ImVec4(line.x + 0.03f, line.y + 0.03f, line.z + 0.03f, 1.0f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(1, 1, 1, 0.022f);   // banding, barely there on purpose

    c[ImGuiCol_TextLink]              = text;
    c[ImGuiCol_InputTextCursor]       = text;
    c[ImGuiCol_TextSelectedBg]        = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_DragDropTarget]        = accent;
    c[ImGuiCol_NavHighlight]          = accent;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1, 1, 1, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(sunken.x, sunken.y, sunken.z, 0.60f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.0f, 0.0f, 0.0f, 0.65f);
}
#endif

// One captured log line for the Output Log panel. `rows` is how many text rows it draws as (Output Log only).
struct LogLine { LogLevel level; std::string text; u32 rows = 1; };

// How many graph prints the on-screen feed keeps. Small on purpose: it is a feed, not a log --
// the Output Log already holds every one of these, without a fade.
constexpr usize kMaxGraphPrints = 12;
// How long a graph print stays on screen, and how much of that is spent fading out.
constexpr f64 kGraphPrintHoldSec = 4.0;
constexpr f64 kGraphPrintFadeSec = 1.0;

// One entry in a Content Browser listing, with everything the views need already derived.
struct DirEntry {
    std::filesystem::path path;
    std::string full, name;
    bool isDir  = false;
    int  tile   = -1;      // sprite tile index, -1 for none
    bool module = false;
    // Glyph when sprite missing (resolved at listing, not draw, for perf).
    int kind = -1;
    std::string kindExt;   // lower-cased extension, for assetKindFor at draw time
    // Graph DOMAIN record (only when kindExt == ".ocgraph").
    editor::GraphAssetFamily graphFamily = editor::GraphAssetFamily::Gameplay;
};

// A Content Browser directory listing, refreshed on a frame stamp. Folders sort first and are counted.
struct DirListing { int stamp = -1000; std::vector<DirEntry> entries; usize dirCount = 0; bool watched = false; };

// Placed mesh animation (anim/animspeed/animtime/animonce from PLACE record).
struct EntityAnim {
    std::string clip;
    f32  speed = 1.0f;
    f32  time  = 0.0f;     // start time t0, seconds
    bool once  = false;
    bool operator==(const EntityAnim&) const = default;
};

// Sanitise filename (remove Windows-invalid chars).
static inline void sanitiseAssetName(std::string& s) {
    for (char& c : s)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    if (s.empty()) s = "unnamed";
}

// glTF import asset counts written to destDir.
// The shape moved to ModelImportDialog.hpp when FBX started sharing the writer; the old name stays for existing callers.
using GltfImportSummary = ModelImportSummary;

// Convert glTF/GLB to .ocmesh/.ocskel/.ocanim in destDir.
bool importGltfToDir(const std::string& src, const std::string& destDir, const std::string& contentDir,
                     bool overwrite, GltfImportSummary& out, std::string* outWhy);

// Forward-declared for handleOpenRequest.
bool isLevelFile(const char* p);
std::string ownerProjectOf(const std::string& mapPath);

// G-buffer debug view: visualise unsampled buffers via overlayPass after tonemap.
static const char* gbufferDebugShaderSource();

class GBufferDebugFeature final : public rhi::IRenderFeature {
public:
    enum class Mode : u32 { Off = 0, Velocity = 1, ViewZ = 2, NormalRoughness = 3 };

    // Velocity debug saturation (texels/frame).
    static constexpr f32 kVelocityFullScaleTexels = 8.0f;
    // ViewZ debug white distance (cm).
    static constexpr f32 kViewZDebugFarUnits = 5000.0f;

    const char* name() const override { return "GBufferDebug"; }

    // Device borrowed (outlives feature for run).
    void setDevice(rhi::IDevice* dev) { device_ = dev; }
    void setMode(Mode m) { mode_ = m; }
    // 3D viewport rect (not whole window, to avoid chrome).
    void setViewportRect(u32 x, u32 y, u32 w, u32 h) { vpX_ = x; vpY_ = y; vpW_ = w; vpH_ = h; }

    ~GBufferDebugFeature() override { releaseGpu(); }

    void overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) override {
        if (mode_ == Mode::Off || !device_ || vpW_ == 0 || vpH_ == 0) return;
        const rhi::TextureHandle tex = textureForMode(mode_);
        if (!tex) {
            // 0 is the documented "nothing here" answer (RHI.hpp) when gBufferEnabled() is false or
            // this backend hasn't implemented the accessor -- an honest no-op, so warn once only.
            if (!warnedMissing_) {
                AVER_WARN("[GBufferDebug] mode {} selected but the device returned no texture for it "
                          "-- is the G-buffer actually enabled, and does this backend implement it yet?",
                          static_cast<u32>(mode_));
                warnedMissing_ = true;
            }
            return;
        }
        if (!ensurePipeline()) return;
        if (tex != boundTex_) { res_->setSrv(binding_, 0, tex); boundTex_ = tex; }

        // Constant buffer: mode, velocity scale, viewZ far, unused.
        const f32 cb[4] = { static_cast<f32>(static_cast<u32>(mode_)), kVelocityFullScaleTexels,
                            kViewZDebugFarUnits, 0.0f };
        // Narrow to 3D viewport only.
        ctx.setViewport(vpX_, vpY_, vpW_, vpH_);
        ctx.setScissor(vpX_, vpY_, vpW_, vpH_);
        ctx.setPipeline(pipeline_);
        ctx.setBindingSet(binding_);
        ctx.setConstantBuffer(kGBufferDebugConstantRegister, cb, sizeof(cb));
        ctx.drawFullscreen();
        // Restore to full backbuffer.
        ctx.setViewport(0, 0, width, height);
        ctx.setScissor(0, 0, width, height);
    }

    // Release GPU resources (idempotent).
    void shutdown() { releaseGpu(); }

private:
    rhi::TextureHandle textureForMode(Mode m) const {
        switch (m) {
            case Mode::Velocity:        return device_->gBufferVelocityTexture();
            case Mode::ViewZ:           return device_->gBufferViewZTexture();
            case Mode::NormalRoughness: return device_->gBufferNormalRoughnessTexture();
            default:                    return 0;
        }
    }

    // Lazy pipeline creation (fullscreen-triangle, G-buffer SRVs).
    bool ensurePipeline() {
        if (pipeline_) return true;
        if (!device_) return false;
        rhi::IResourceFactory* res = device_->resources();
        if (!res) return false;
        res_ = res;

        const char* src = gbufferDebugShaderSource();
        rhi::ShaderDesc vsd; vsd.source = src; vsd.entry = "VSGBufferDebug";
        vsd.stage = rhi::ShaderStage::Vertex; vsd.minShaderModel = 51;
        rhi::ShaderDesc psd; psd.source = src; psd.entry = "PSGBufferDebug";
        psd.stage = rhi::ShaderStage::Pixel; psd.minShaderModel = 51;
        const rhi::ShaderHandle vs = res->createShader(vsd);
        const rhi::ShaderHandle ps = res->createShader(psd);

        bool ok = false;
        if (vs && ps) {
            rhi::GraphicsPipelineDesc pd;
            pd.vs = vs; pd.ps = ps;
            pd.layout.srvCount = 1;
            pd.layout.samplers[0] = rhi::SamplerDesc{rhi::Filter::Linear, rhi::AddressMode::Clamp};
            pd.layout.samplerCount = 1;
            pd.cull = rhi::CullMode::None;
            pd.depthClip = false;
            pd.renderTargetCount = 1;
            // The display target (tonemapped composite), NOT device_->backbufferFormat() (the HDR scene).
            pd.renderTargets[0] = device_ ? device_->displayFormat() : rhi::Format::RGBA8Unorm;
            pd.sampleCount = 1;   // Already-resolved composite.
            pipeline_ = res->createGraphicsPipeline(pd);
            ok = pipeline_ != 0;
        }
        if (vs) res->destroyShader(vs);
        if (ps) res->destroyShader(ps);
        if (!ok) { AVER_ERROR("[GBufferDebug] pipeline unavailable"); return false; }

        rhi::BindingSetDesc bd;
        bd.srvCount = 1;
        bd.srvKinds[0] = rhi::SlotKind::Texture2D;
        binding_ = res->createBindingSet(bd);
        if (!binding_) {
            AVER_ERROR("[GBufferDebug] binding set unavailable");
            res->destroyPipeline(pipeline_);
            pipeline_ = 0;
            return false;
        }
        return true;
    }

    void releaseGpu() {
        if (res_) {
            if (pipeline_) res_->destroyPipeline(pipeline_);
            if (binding_)  res_->destroyBindingSet(binding_);
        }
        pipeline_ = 0; binding_ = 0; boundTex_ = 0;
    }

    // Own CBV register (no shared prelude).
    static constexpr u32 kGBufferDebugConstantRegister = 1;

    rhi::IDevice*          device_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    Mode mode_ = Mode::Off;
    u32 vpX_ = 0, vpY_ = 0, vpW_ = 0, vpH_ = 0;
    rhi::PipelineHandle   pipeline_ = 0;
    rhi::BindingSetHandle binding_  = 0;
    rhi::TextureHandle    boundTex_ = 0;
    bool warnedMissing_ = false;
};

// GBuffer debug shader (fullscreen-triangle, own constants).
static inline const char* gbufferDebugShaderSource() {
    // Keyed on shaderFileRevision(), not a plain static -- the loader owns the cache.
    static std::string s;
    static u64 built = ~0ull;
    if (built != rhi::shaderFileRevision()) {
        s = rhi::shaderFile("gbuffer_debug.hlsl");
        built = rhi::shaderFileRevision();
    }
    return s.c_str();
}

// NeuraFI visualisation overlay (over 3D viewport, post-tonemap, alpha-blended).
class NeuraFiVizFeature final : public rhi::IRenderFeature {
public:
    const char* name() const override { return "NeuraFiViz"; }
    ~NeuraFiVizFeature() override { releaseGpu(); }

    void setDevice(rhi::IDevice* dev) { device_ = dev; }
    // Set source, vizCount baseline, opacity (called pre-render).
    void setSource(const aver::neurafi::NeuraFI* src, f32 opacity) {
        src_ = src;
        baseline_ = src ? src->vizCount() : 0;
        opacity_ = opacity;
    }
    void setViewportRect(u32 x, u32 y, u32 w, u32 h) { vpX_ = x; vpY_ = y; vpW_ = w; vpH_ = h; }

    void overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) override {
        if (!src_ || !device_ || vpW_ == 0 || vpH_ == 0 || src_->vizCount() == baseline_) return;
        const rhi::TextureHandle tex = src_->visualisation();
        if (!tex || !ensurePipeline()) return;
        if (tex != boundTex_) { res_->setSrv(binding_, 0, tex); boundTex_ = tex; }
        const f32 cb[4] = {opacity_, 0.0f, 0.0f, 0.0f};
        ctx.setViewport(vpX_, vpY_, vpW_, vpH_);
        ctx.setScissor(vpX_, vpY_, vpW_, vpH_);
        ctx.setPipeline(pipeline_);
        ctx.setBindingSet(binding_);
        ctx.setConstantBuffer(kConstantRegister, cb, sizeof(cb));
        ctx.drawFullscreen();
        ctx.setViewport(0, 0, width, height);
        ctx.setScissor(0, 0, width, height);
    }

    void shutdown() { releaseGpu(); }

private:
    bool ensurePipeline() {
        if (pipeline_) return true;
        if (failed_ || !device_) return false;
        rhi::IResourceFactory* res = device_->resources();
        if (!res) return false;
        res_ = res;
        const std::string& src = rhi::shaderFile("neural_visualiser.hlsl");
        if (src.empty()) {
            AVER_ERROR("[NeuralViz] neural_visualiser.hlsl is not deployed beside the executable");
            failed_ = true;
            return false;
        }
        rhi::ShaderDesc vsd; vsd.source = src.c_str(); vsd.entry = "VSNeuralViz";
        vsd.stage = rhi::ShaderStage::Vertex; vsd.minShaderModel = 51;
        rhi::ShaderDesc psd; psd.source = src.c_str(); psd.entry = "PSNeuralViz";
        psd.stage = rhi::ShaderStage::Pixel; psd.minShaderModel = 51;
        const rhi::ShaderHandle vs = res->createShader(vsd);
        const rhi::ShaderHandle ps = res->createShader(psd);
        if (vs && ps) {
            rhi::GraphicsPipelineDesc pd;
            pd.vs = vs; pd.ps = ps;
            pd.layout.srvCount = 1;
            pd.layout.samplers[0] = rhi::SamplerDesc{rhi::Filter::Linear, rhi::AddressMode::Clamp};
            pd.layout.samplerCount = 1;
            pd.cull = rhi::CullMode::None;
            pd.depthClip = false;
            pd.renderTargetCount = 1;
            pd.renderTargets[0] = device_ ? device_->displayFormat() : rhi::Format::RGBA8Unorm;   // the display target
            pd.sampleCount = 1;
            pd.blend = rhi::BlendMode::AlphaBlend;
            pipeline_ = res->createGraphicsPipeline(pd);
        }
        if (vs) res->destroyShader(vs);
        if (ps) res->destroyShader(ps);
        if (pipeline_) {
            rhi::BindingSetDesc bd;
            bd.srvCount = 1;
            bd.srvKinds[0] = rhi::SlotKind::Texture2D;
            binding_ = res->createBindingSet(bd);
        }
        if (!pipeline_ || !binding_) {
            AVER_ERROR("[NeuralViz] pipeline unavailable");
            releaseGpu();
            failed_ = true;
            return false;
        }
        return true;
    }

    void releaseGpu() {
        if (res_) {
            if (pipeline_) res_->destroyPipeline(pipeline_);
            if (binding_)  res_->destroyBindingSet(binding_);
        }
        pipeline_ = 0; binding_ = 0; boundTex_ = 0;
    }

    static constexpr u32 kConstantRegister = 1;   // Own constant register.

    rhi::IDevice*          device_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    const aver::neurafi::NeuraFI* src_ = nullptr;
    u64 baseline_ = 0;
    f32 opacity_ = 1.0f;
    u32 vpX_ = 0, vpY_ = 0, vpW_ = 0, vpH_ = 0;
    rhi::PipelineHandle   pipeline_ = 0;
    rhi::BindingSetHandle binding_ = 0;
    rhi::TextureHandle    boundTex_ = 0;
    bool failed_ = false;
};

#include "state/EditId.hpp"
#include "state/PlayState.hpp"
#include "state/HarnessState.hpp"
#include "state/RenderSettingsState.hpp"
#include "state/ViewportState.hpp"
#include "state/LandscapeToolState.hpp"
#include "state/DrawerState.hpp"
#include "state/SelectionState.hpp"
#include "state/OcclusionState.hpp"
#include "state/LodState.hpp"
#include "state/RevisionControlState.hpp"
#include "state/McpState.hpp"
#include "state/AutosaveSession.hpp"
#include "state/DebugOverlayState.hpp"
#include "state/PanelState.hpp"
class HitchMarks;
namespace game { struct SceneDrawStats; }

// The editor application: owns the scene, the panels, and the frame loop.
class SandboxApp final : public Application {
public:
    SandboxApp(u64 maxFrames, bool headless, std::string beamPath, std::string shot, Tool initialTool);

    static std::string splashTextFor(std::string_view msg);

    static void logSink(void* ctx, LogLevel level, std::string_view msg);

    BootConfig config() const override;
    bool startupComplete() const override;

    void setUseWarp(bool w);
    void setWindowed(bool w);                          // --windowed (now the default)
    void setFullscreen(bool f);                      // --fullscreen
    void setBackend(std::string b);   // --backend <name>
    void setCultureOverride(std::string tag);   // --culture <tag>: the UI culture over the preview pref (docs/LOCALIZATION.md)
    void setFrameBudget(f32 ms);
    void setDebugLayer(bool d);

#if AVER_WITH_IMGUI
    void applyDpi(f32 dpi);

    void mergeIconFont(f32 px);

    void loadLogo(Engine& e);

    void loadCompileIcon(Engine& e);

    void loadGameUiFont(Engine& e);

    bool loadIconSheet(Engine& e, const char* file, int tiles, const char* debugName,
                       rhi::TextureHandle& outTex, u64& outId, f32& outAspect);
#endif

#if AVER_WITH_IMGUI
    void onDeviceCreated(Engine& e) override;
#endif

    void onInit(Engine& e) override;

    void onUpdate(Engine& e, const Timestep& t) override;

#if AVER_MODULE_VOXI && AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // ---- particles DECIDED 4: two halves of particles::ParticleRenderer::GiSeam ----
    // Both static (like resolveMaterialTexture/depthProxyLookup): read voxiRenderer_'s members only
    // through `user`. modules/particles never sees this file or that voxi is on the other end of its
    // GiSeam -- these two functions are that entire boundary.

    static bool particleGiPrepare(u32 srvBase, u32 samplerBase, u32 cbRegister,
                                  std::string* outPrelude, std::string* outDefines, void* user);

    static void particleGiBind(rhi::IResourceFactory& res, rhi::BindingSetHandle set, u32 srvBase,
                               const void** outCbData, u32* outCbBytes, void* user);
#endif  // AVER_MODULE_VOXI && AVER_MODULE_PARTICLES && AVER_MODULE_SCENE

    // ---- mesh loading ----

    // Load project meshes and build editor tables.
    void loadProjectMeshes(Engine& e);
#if AVER_MODULE_SCENE
    // Mesh load pass context.
    struct MeshLoadPass {
        SandboxApp* app = nullptr;
        Engine* engine = nullptr;
        u32 lodCoarserLevels = 0;
        u32 lodSharedLevels = 0;
        u64 lodSharedVertexBytesSaved = 0;
        bool streamed = false;   // level streaming's uploads: no per-mesh log lines
    };
    // Per-mesh load callback (user = MeshLoadPass).
    static void onMeshLoaded(const game::GameContent::LoadedMesh& mesh, void* user);
#endif

#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    void ensureLodMeshPipeline(Engine& e);
#endif

    void releaseProjectMeshes(Engine& e);

    // Load materials from Binaries and Content directories.
#if AVER_MODULE_PBR
    void loadProjectMaterials();

    void releaseProjectMaterials();

    // ---- level-scoped material residency ----
    // True = keep only open level's materials resident (vs. all materials in Binaries/Content).
    // Release half: unloadLevel checks this; bind half: GameLevel::load re-resolves surfaces.
    static bool levelScopedMaterialsEnabled();

    // Materials to preserve across level changes (e.g., editor tabs with open materials).
    void pinMaterialResident(const std::string& name) { pinnedMaterialNames_.insert(name); }
    void unpinMaterialResident(const std::string& name) { pinnedMaterialNames_.erase(name); }
#endif


    void makeMaterialFor(MeshObj& o);

    void onRender(Engine& e) override;

    void skinDrawCheck(Engine& e);

    void submitGameUi(Engine& e);

    void drawUiDemo();

    int exitCode() const override;

    void onShutdown(Engine& e) override;
    void setVSyncOff(bool off);               // --no-vsync
    void setVSyncOn(bool on) { rset_.vsyncOnRequested_ = on; }   // --vsync
    void setLodSelect(bool on, f32 thresholdPx);
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_VOXI
    void applyScriptRender(voxi::Renderer& vx);   // framework_render_abi.h at the frame boundary (SandboxUpdate.cpp)
#endif
    void setLodClusterStats(bool on);
    void setLodPerCluster(bool on, f32 thresholdPx);
    void setLodMeshShader(bool on, f32 thresholdPx);
    void setUiDemo(bool on);                          // --ui-demo
    void setOpenAsset(std::string p);        // --open-asset
    void setSelectEntity(std::string p);
    void setWater(bool on, f32 heightCm);
    void setOpenLegacy(bool on);
    void setGraphSelectNode(std::string id);
    void setGraphTab(std::string tab);
    void setInputProbe(bool on);
    void setAutoCompile(bool on);   // --auto-compile, and the Tools menu
    void setFocusLevelAt(int frame);   // --focus-level-at <N>
    void setChunkStreamAuto(int framesIn);
    // --drone-graph <relPath>
#if AVER_MODULE_SCENE
    void setDroneGraph(std::string relPath);
#else
    void setDroneGraph(std::string);
#endif

    void setLandscapePath(std::string path);

    // --fog-match (negative opacity = keep current).
#if AVER_MODULE_SCENE
    void setFogMatchToStreamRadius(bool on, f32 targetOpacity);
#else
    void setFogMatchToStreamRadius(bool, f32);
#endif
    void setDroneAuto(int framesIn);
    void setUndoTestAuto(int framesIn);
    void setClassPlaceTestAuto(int framesIn);   // --class-place-test (SandboxSelfTests.cpp)
    void setKeybindTestAuto(std::string mode, int framesIn);
    void setShowEditorPrefs(bool on);   // --editor-prefs
    void setScrollPrefsToKeybinds(bool on);
    void setHudTest(int idx);   // --hud-preview <index>
    void setSaveProject(bool on);   // --save-project
    void setSaveLevelTo(std::string p);   // --save-level <out>
    void setBakeLevelPack(std::string levelAsset);   // --bake-level-pack <content-relative level>  [wave3/level-instances]
    void setRayProbe(f32 x, f32 y);   // --ray-probe
    void setImportOnce(std::string src, std::string dst);
    bool* autoCompileFlag();

    void setClouds(f32 coverage);
    void setSkyPhysical(f32 elevationDeg);
    void setSkyAuthored();   // --sky-authored
    void setPost(f32 exposure, bool exposureSet, f32 bloomIntensity, bool bloomSet, bool autoExposure);

    void setTonemap(int mode);
    void setMaxRadiance(f32 ceil);

    void applyCaptureExposureRule(bool explicitlyRequested);
    void setFocusVoxi(bool b); // --project-settings
    void setProjectSettingsPage(int page);
    void setDrawerOpen(int which, std::string sub);
    void setFocusScript(bool b);  // --new-script
    void setFocusTools(bool b);   // --tools-menu
    void setOpenLevelPicker(bool b);   // --open-level-picker
    void setNoEditorChrome(bool b);        // --no-editor-chrome
    void setSceneCensus(bool b);              // --scene-census
    void setOpenLevelByName(std::string n);   // --open-level
    void setFocusCompileMenu(bool b);   // --compile-menu
#if AVER_MODULE_MCP
    void setMcpPort(u16 p);
#endif
    void setSkinTest();                                       // --skin-test
    void setSkinDrawTest();                               // --skin-draw-test
    void setParticleTest();                               // --particle-test
    void setNoParticleGi();                               // --no-particle-gi
    void setParticleStress(int emitters, int maxParticles);
    void setParticleStressSecondEmitter();   // --particle-stress2
    void setReflTest();                                       // --refl-test
    void setFurnaceTest();                                 // --furnace-test
    void setFurnaceSun();              // --furnace-sun
    void setFurnaceGrid();            // --furnace-grid
    void setFurnaceTilt(f32 d);
    void setSunAngle(f32 deg);
    void setPtFurnaceTest();        // --pt-furnace
    void setPtSceneView();   // --pt-scene
    void setPtQualityRamp(int everyFrames);
    void setGiMethodCycle(int everyFrames) { harness_.giMethodCycleEvery_ = everyFrames; harness_.giMethodCycleCountdown_ = everyFrames; }
    void setPtSceneToggleOnAuto(int framesIn);
    void setPtSceneToggleOffAuto(int framesIn);
    void setSunSetAt(int framesIn, f32 elevDeg, f32 azimDeg);
    void setSunSweep(int framesIn, f32 degPerFrame, int turns);
    void setGiHistoryResetAt(int framesIn);
#if AVER_MODULE_SR
    void setAverSrCycleAuto(int framesIn);   // --aversr-cycle [N]
#endif
    void setResizeCycle(int n);              // --resize-cycle [N]
    void setFrameInterpCli(int on) { rset_.frameInterpCli_ = on; }   // --frame-interp 0|1|2 (2: capture the generated image)
    void setFrameInterpTrajectory(int t, bool train) { rset_.frameInterpTrajectoryCli_ = t; rset_.frameInterpTrainCli_ = train; }
    // --neurafi-view, --neurafi-generated-only, --neurac-view, --neurac-grid (negative = not given).
    void setNeuralVisualiserCli(int neurafiView, bool generatedOnly, int neuracView, bool neuracGrid) {
        if (neurafiView >= 0) dbgOv_.neurafiVizMode_ = neurafiView;
        if (generatedOnly) dbgOv_.neurafiShowGeneratedOnly_ = true;
        if (neuracView >= 0) dbgOv_.neuracViewMode_ = neuracView;
        if (neuracGrid) dbgOv_.neuracViewGrid_ = true;
    }
    void setGpuTiming(bool on);                                // --gpu-timing
    void setLumaSweep(bool on, int stride);
    void setFireflyMetric(bool on, f32 mult);
    void setPieCameraTest(int n);                            // --pie-camera-test
    void setInputStuckTest(int n);                       // --input-stuck-test
    void setInputSourceTest(int n);                        // --input-source-test
    void setWheelSpeedTest(int n);                        // --wheel-speed-test
    void setMultiSelectTest(int n);                    // --multiselect-test
    void setCbMoveTest(const std::string& dir);
    void setSaveDirtyTest(int n);                     // --savedirty-test
    void setPrefsWriteTest(int n);                   // --prefs-write-test
    void setNotifyTest(int n);
    void setAutosaveTest(f32 sec);
    void setFindRefs(const std::string& p);
    void setRenameRepointTest(int frames);
    void setProjectSwitchTest(int frames);
    void setValidateGraph(const std::string& p);
    void setGraphPrintTest(int frames);
    void setAssetAssignTest(int frames);
    void setGraphHitsTest(const std::string& p);
    void setClearShaderCache(const std::string& dir = {});
    void setRecaptureTest(int n);                          // --recapture-test
    void setViewmodelTest(int n);                             // --viewmodel-test
    void setSkinSceneDir(std::string d);          // --skin-scene-test <dir>
    void setShaderSourceDir(std::string d);
    // ---- non-navigation features ----
#if AVER_MODULE_PHYSICS
    // Rebuild physics collider overlay (returns early if unchanged).
    void rebuildColliderOverlay(Engine& e);
#endif
#if AVER_WITH_IMGUI
    void buildReferencesPanel();

    void buildProfilerPanel(Engine& e);
    // Neural visualiser panel (NeuraFI, NeuRaC).
    void buildNeuralVisualiserPanel(Engine& e);
#endif
    // Reset profiler measurements (play start).
    void resetPlayProfile(bool playStarting);

#if AVER_MODULE_SYNAPSE
    void setBakeNavOnStart(f32 cellCm);   // --bake-nav [cm]

    // Rebuild nav overlay (release old before creating new).
    void rebuildNavOverlay(Engine& e);

    // Guarded on SCENE (SYNAPSE is grid math only).
#if AVER_MODULE_SCENE
    bool bakeNavigationNow(Engine& e, std::string* why = nullptr);
#endif

    void loadNavForLevel(Engine& e);
#endif
    void setFocusCompile(bool b);   // --compile-scripts
    void setFocusReload(int frames); // --reload-scripts [N]
    void warnDeadMaterialHandle(i32 mat);

    // Material surface resolver (centralised from three drifting copies).
    struct ResolvedSurface {
        aver::game::SurfaceLook look;
        u32 authored = 0;                    // pbr::MaterialLibrary handle, or 0 (built-in look/fallback)
        rhi::BindingSetHandle matSet = 0;
        const void* matConstants = nullptr;   // reference into MaterialSystem's storage; see resolveSurface's contract
        u32 matBytes = 0;
    };

    // Two call sites remain: cluster mesh-shader and editor warnings.
    ResolvedSurface resolveSurface(i32 mat);

#if AVER_MODULE_SCENE
    rhi::MeshHandle posedHandle(scene::Entity ent);
#endif

    const char* occlusionSuppressingFeatureName() const;

    void setMsaaOverride(int n);   // --msaa N
    void setDepthPrepassOverride(bool on);   // --depth-prepass
    void setGBufferOverride(bool on);             // --gbuffer
    void setGBufferDebugView(GBufferDebugFeature::Mode m);
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
    void setOcclusionCullOverride(bool on);
    void setOcclusionCullForceOff();
    void setOcclusionDebugForceWaitIdle(bool on);
#endif
    void setGiOverride(int q, bool dbg); // --gi / --gi-debug
    void setGiForceOff(bool off);                        // --no-gi
    void setGiConeTraceOff(bool off);                // --no-gi-cone
    void setRtOverride(int q);                              // --rt
    void setPtOverride(int q);                              // --pt
    void setRtForceOff(bool off);                         // --no-rt
    void setRtRays(int n);                              // --rt-rays N
    void setGiSkyOcclusionRays(int n);             // --gi-sky-occlusion-rays N
    void setGiSkyOcclusionTile(int n);             // --gi-sky-occlusion-tile N
    void setSkyLight(f32 v);                          // --sky-light N
    void setGiIntensity(f32 v);                    // --gi-intensity F
    void setRtPixelsPerRay(int n);              // --rt-pixels-per-ray N
    void setRtShadowDenoise(int n);            // --rt-shadow-denoise N
    void setRtRenderMode(int n);                  // --rt-render-mode 0|1
    void setRdStages(int n);                      // --rd-stages 0|1|2
    void setPtBounces(int n);                        // --pt-bounces N
    void setLayeredBsdf(int n);                    // --layered-bsdf N
    void setCoat(f32 w, f32 r, f32 f0);   // --coat W [R] [F0]
    void setGiUpdateInterval(int n);          // --gi-update-interval N
    void setGiMode(int n);                             // --gi-mode N
    void setRestirVisibility(int n);
    void setDenoiser(int n);                          // --denoiser 0|1
    void setRenderScale(f32 s);                    // --render-scale F
#if AVER_MODULE_SR
    void setAverSrQuality(aver::sr::Quality q);

    void setAverSrCliAuto();

    void ensureAverSrUpscaler(rhi::IDevice* dev);

    void clearAverSrUpscaler(rhi::IDevice* dev);

    void setEdgeAaOverride(bool on);   // --edge-aa
    void setTaaOverride(bool on) { rset_.temporalAaEnabled_ = on; rset_.taaFromCli_ = true; }   // --taa / --no-taa

    void setParticleGpuOverride(int mode) { rset_.particleGpuArg_ = mode; }                // --particles-cpu (0) / --particles-gpu (2)
    void setNeuRaaOverride(bool on) { rset_.neuraaEnabled_ = on; rset_.neuraaFromCli_ = true; } // --neuraa / --no-neuraa

    void applyUpscalerSlot(rhi::IDevice* dev);

    void logAverSrActive(rhi::IDevice* dev);

    void applyAverSrQuality(rhi::IDevice* dev, aver::sr::Quality q);

    // AverSR + Voxi level functions (depend on Voxi ladder types).
#if AVER_MODULE_VOXI
    const char* averSrSourceText(voxi::AverSrSource source) const;

    const char* averSrAutoRungName(const voxi::Settings& s, const voxi::DeviceInfo& d) const;

    void updateAverSrAuto(Engine& e);
#endif  // AVER_MODULE_VOXI
#endif  // AVER_MODULE_SR

    void syncPtSceneView(rhi::IDevice* dev);
    // Path Tracing on owns the viewport, unless a view mode needs the raster or ray-driven path.
    bool ptTakesViewport() const;

    void setFrameTimeReport(bool on);                 // --frame-time
    void setRayDrivenAblation(int m);
    void setRtDenoiseMotionTaper(f32 v);
    void setRefractionOverrides(int mode, f32 strength, f32 fade);
    void setMsOverride(bool on);                           // --ms
    void setProbe(u32 x, u32 y);                    // --probe X Y
    void setProbeRel(f32 u, f32 v);                 // --probe-rel U V
    void setCamWobble(f32 degrees, i32 periodFrames);
    // --cam-wobble-stop, --set: console variables and camera control.
    void setCamWobbleStop(i32 frame);
    void addCamTeleport(i32 frame, const Vec3& pos) { harness_.camTeleports_.push_back({frame, pos}); }
    void setConsoleSets(std::vector<std::pair<std::string, std::string>> sets);
    void setMeshHeapDefault(bool on);
    void setLodShareVertices(bool on);
    void setCamTranslate(f32 speedCmPerFrame);
    void setCamWander(f32 amp, f32 speed);        // --cam-wander AMP SPEED
    void setCamFly(f32 dx, f32 dy, f32 yawDeg) { harness_.camFlyX_ = dx; harness_.camFlyY_ = dy; harness_.camFlyYaw_ = yawDeg; }   // --cam-fly
    // --nrd2-capture DIR POSES [HOLD] [HELDOUT_FROM] and --nrd2-oracle grad|grid (NRD2 phase 3 dataset).
    // DIR "default" (or "-") is %LOCALAPPDATA%/AverEngine/nrd2_dataset/<level>.
    void setNrd2Capture(const std::string& dir, u32 poses, u32 hold, u32 heldOutFrom, bool oracleGrid) {
        harness_.nrd2CaptureDir_ = dir; harness_.nrd2CapturePoses_ = poses; harness_.nrd2CaptureHold_ = hold;
        harness_.nrd2CaptureHeldOut_ = heldOutFrom; harness_.nrd2CaptureGrid_ = oracleGrid;
    }
    // --nrd2-train STEPS [DATASETDIR...] (NRD2 phase 4; no dirs = %LOCALAPPDATA%/AverEngine/nrd2_dataset).
    void setNrd2Train(u32 steps, std::vector<std::string> dirs) { nrd2Session_.requestCli(steps, std::move(dirs)); }
#if AVER_MODULE_SR
    void setNeuRaaCapture(const std::string& dir, u32 count) { rset_.neuraaCaptureDir_ = dir; rset_.neuraaCaptureCount_ = count; }
#endif
    void setCamera(Vec3 pos, f32 pitchDeg, f32 yawDeg);
    void setScriptsDir(std::string d);            // --scripts <dir>
    void setSpawnTest(std::string cls);      // --spawn-test <ClassName>
    void setUnlitMode(bool on);                                  // --unlit
    // --view-mode lit|unlit|wireframe|rayhit-*|triangles|undenoised (applied at startup).
    void setViewMode(const std::string& mode);
    void setPlayTest();                                       // --play-test
    void setPlayWalk() { play_.defaultPawnWalk_ = true; }          // --play-walk: the no-GameMode default pawn walks
    void startDefaultPawnWalk(bool fromCamera);                // SandboxPlay.cpp: the capsule for a walking default pawn
    void driveDefaultPawnWalk(const Vec3& fwd, const Vec3& right);   // per frame, from the fly block
    void startDefaultPawnFly();                                // SandboxPlay.cpp: the flying default pawn's collider
    void driveDefaultPawnFly(const Vec3& velocity, f32 dt);    // per frame; dt 0 = no drift (hard set)
    void setProjectPath(std::string p);          // <path>.ocproject
    void setStartMode(std::string m);
    // --sequence-play [--sequence-capture dir] [--sequence-every N] [--sequence-warmup N]: SandboxSequenceRun.cpp.
    struct SequenceRunArgs {
        bool play = false;
        std::string dir;      // numbered PNGs go here; empty = play without capturing
        int every = 1;        // capture every Nth frame
        int warmup = 120;     // frames to wait after the level is ready before frame 0
        std::string job;      // --render-queue <file.ocrender>: a Movie Render Queue job instead of the plain capture
    };
    void setSequenceRun(const SequenceRunArgs& a);
    // The Movie Render Queue (SandboxSequenceRun.cpp, docs/RENDER_QUEUE.md): starts a job in the running
    // editor, relative paths taken from baseDir. False with `why` when one cannot start now.
    bool startRenderJob(const fmt::RenderJob& job, const std::string& baseDir, std::string& why);
    void cancelRenderJob();
    void setOpenMap(std::string p);
    void armBrowser(bool on);   // shows the start screen
    void setSingleInstanceEligible(bool b);

private:
    // Project loading with splash progress screen (avoid "Not Responding").
    struct LoadingScreen {
        Splash splash;
        bool on = false;
        Engine* borrowed = nullptr;   // non-null when reusing the engine's startup splash

        // Borrow engine's splash if still up (cmd-line open); else create new splash.
        LoadingScreen(Engine& eng, bool enable, const std::string& png) {
            if (!enable) return;
            if (eng.loadingScreenActive()) { borrowed = &eng; return; }
            on = splash.show(png);
        }
        void stage(const char* text) {
            if (borrowed) { borrowed->setLoadingStatus(text); return; }
            if (!on) return;
            splash.setStatus(text);
            splash.pump();
        }
        // Stage with completion fraction.
        void stage(const char* text, f32 fraction) {
            stage(text);
            progress(fraction);
        }
        // Update progress bar (Splash::setProgress throttles).
        void progress(f32 fraction) {
            if (borrowed) { borrowed->setLoadingProgress(fraction); return; }
            if (!on) return;
            splash.setProgress(fraction);
        }
        ~LoadingScreen() {
            // No min visible time; borrowed splash closed by Engine.
            if (on) splash.close(0);
        }
    };

    // ---- feature state: plain-data groups moved out of this class (sandbox/src/state/) ----
    PlayState play_;   // Play-in-editor session
    HarnessState harness_;   // Automation and self-test state
    RenderSettingsState rset_;   // Render-settings mirror
    ViewportState viewState_;   // Viewport and camera
    LandscapeToolState landTool_;   // Landscape, foliage and water-spline tools
    DrawerState drawerUi_;   // Bottom drawer
    SelectionState selectionUi_;   // Selection and the outliner
    OcclusionState occl_;   // Software occlusion culling
    LodState lod_;   // Mesh LOD / cluster selection and mesh-shader path
    RevisionControlState revCtl_;   // Revision-control panel
    McpState mcpState_;   // MCP automation bridge
    AutosaveSession autosave_;   // Level/project/prefs autosave timers, crash-recovery state and the post
    DebugOverlayState dbgOv_;   // Debug overlays and diagnostics panels
    PanelState panels_;   // Editor panel/dialog visibility flags, prompt requests and dock layout 
    // ---- end feature state ----

    void applyProject(Engine& e);

    // Autosave/crash-recovery (unguarded, not in landscape block).
    void maybeAutosavePrefs(f32 dt);

    // Multi-material meshes split at load (one material per instance).

    rhi::LineHandle selectionOutlineLines(Engine& e, u64 meshId);

    std::string autosavePathFor(const std::string& levelPath) const;

    void maybeAutosave(f32 dt);

    void autosaveCancelNotice();

    void autosaveRunSave();

    void clearAutosave();

    void checkForRecovery();

    void drawRecoveryPrompt(Engine& e);

    // ---- terrain ----
#if AVER_MODULE_LANDSCAPE
    bool createLandscapeForLevel(rhi::IDevice* device, u32 samples, f32 spacingCm);

    void recordLandscapeInLevel();

    // Wire chunk generator to terrain section.

    void applyLandscapeToStreaming();

    void saveLandscape();
#endif

    void locateAverDesign() const;

    // Returns the directory the CLR host loads user assemblies from: --scripts, else project Binaries\Scripts, else <exe>\Scripts.
#if AVER_MODULE_SCRIPTING
    std::string resolveScriptsDir() const;
#endif

#if AVER_MODULE_VOXI
    void applyProjectVoxiSettings();

    void applyProjectRenderSettings();

    void captureRenderSettingsFromUi(const voxi::Settings& requested, u32 overallFollowMask);
#else
    void applyProjectRenderSettings();
#endif

    void seedAndSaveProject();

    bool saveProjectManifest(std::string* why);


    // Above unguarded (manifest keys); below guarded on VOXI (controller state).
#if AVER_MODULE_VOXI
    voxi::FrameBudgetState frameBudget_;

    void frameBudgetTick(f32 dt, voxi::Settings& vs);
#endif

    void maybeAutosaveProject(f32 dt);
    std::string projectSaveStatus_;

    bool hudPreviewActive() const;
    void setHudPreview(int index, f32 x, f32 y, f32 w, f32 h);
    std::string saveLevelTo_;         // --save-level <out>
    std::string bakeLevelPackAsset_;  // --bake-level-pack <level>  [wave3/level-instances]

    void saveLevelInteractive();

    // Save level and all dirty asset tabs (File > Save All, Ctrl+Shift+S).
    void saveAll();

    // Viewport screenshot to Saved/Screenshots (File > Take Screenshot, F9).
    void requestViewportScreenshot();
    void serviceViewportScreenshot(Engine& e);

    // Window title (updated with level and dirty state).
    void refreshWindowTitle(Engine& e);
    std::string windowTitleShown_;

    void startContentWatch();

    void pumpContentWatch();

    static bool isTextureSource(const std::string& rel);

    static bool isScriptSource(const std::string& rel);

    void scheduleAutoCompile(const std::string& why);

    void serviceAutoCompile();

    DirectoryWatcher contentWatch_;
    std::vector<FileEvent> watchEvents_;

    // Unloads the live script assemblies and loads the ones in binDir. Writes a status line.
#if AVER_MODULE_SCRIPTING
    bool reloadScripts(const std::string& binDir, std::string* status);
#endif

#if AVER_MODULE_FRAMEWORK
    void maybeSpawnTestActor();

    void capturePlayWorld();

    void restorePlayWorld();

    int32_t engineDefaultGameMode();
    // World/Project Settings: a combo of declared classes with `flag`, by name (SandboxSettings.cpp).
    bool classPicker(const char* label, std::string& value, int32_t flag, const char* emptyLabel,
                     const char* engineClass, const char* engineLabel);

    bool placePawnAtPlayerStart(const char* who);

    void startPlay();

    bool dronePlayActive() const;
    bool spectatorPlayActive() const;

    bool anyPlayActive() const;

    void stopPlay();

    void maybePieCameraTest();

    void maybeInputSourceTest();

    void maybeWheelSpeedTest();

    void maybeViewmodelTest();

    void maybeRecaptureTest();

    void maybeInputStuckTest();

    void maybePlayTest(Engine& e);

    void pushInput(bool uiActive);

    void drivePlayCamera(f32 dt);
#endif

    f32 viewAspect() const;
    bool inViewport(f32 mx, f32 my) const;
    Vec3 camForward() const;

#if AVER_MODULE_SCENE
    void multiRebuildSet();                     // after any bulk rewrite of multiSel_

    bool multiStale() const;
    bool multiIsSelected(scene::Entity e) const;
    void multiSyncToAnchor();
    void multiClear();
    void multiSetSingle(scene::Entity e);
    void multiToggle(scene::Entity e);
    void multiRange(scene::Entity to);
    std::vector<scene::Entity> selectedEntities() const;

    void selectAllInOutliner();
#endif

    bool movableSelected() const;

    bool gameHasInput() const;

    // The gizmo's world-agnostic view of a selection's transform. Rotation is in Euler degrees.
    struct EditXform { Vec3 pos, rotDeg, scale; };

    bool anySelected() const;

    bool selectedXform(EditXform& x) const;

    void setSelectedXform(const EditXform& x);

#if AVER_MODULE_SCENE
    static Transform localFromWorldFor(scene::World& w, scene::Entity e, const EditXform& x);

    static Transform worldTransformOf(scene::World& w, scene::Entity e);
#endif

    // Stable identity for an undoable object, so a command survives the entity being recreated.
    using EditId = ::aver::EditId;   // declared at namespace scope (state/EditId.hpp) so the state structs can name it

    // One undoable edit (transform, create/destroy). Components captured via EntitySnapshot.
    struct EditCmd {
        enum class Kind { Transform, Create, Destroy, CreateObj, DestroyObj, LandscapeStroke, FoliageStroke, Reparent, Material, Rename, RemoveComponent, Visibility, Collision, Animation, Prefab, LevelInstance, LandscapeWeights, WaterBodies, PlacedComponent, OutlinerMeta, DataLayers };
        Kind kind = Kind::Transform;
        // Monotonic serial for save points; never reused so undo/redo cross saves correctly.
        u64 serial = 0;
        EditId id = 0;            // a scene entity, through the indirection
        int objIndex = -1;        // or an objects_ index, for the placeholder scene
        EditXform before{}, after{};

        // Other entities moved with anchor as one command (avoids "undo partway" inconsistency). Stored as LOCAL transforms.
        // root: the entity had no parent when recorded, so its local is in the scene frame and a floating-origin shift moves it.
        struct AlsoMoved { EditId id = 0; Transform beforeLocal, afterLocal; bool root = true; };
        std::vector<AlsoMoved> alsoMoved;

        // VISIBILITY: before/after per entity, replayed through setAuthoredVisible().
        struct VisibilityChange { EditId id = 0; bool before = true; bool after = true; };
        std::vector<VisibilityChange> visibility;
        // COLLISION: before/after per entity, replayed through setEntityCollide().
        std::vector<VisibilityChange> collide;
        // ANIMATION: before/after per entity (empty clip = none), replayed through applyEntityAnim().
        struct AnimChange { EditId id = 0; EntityAnim before, after; };
        std::vector<AnimChange> animation;
        std::string label;        // outliner display name; editor-owned bookkeeping, not World's
        // OUTLINERMETA: lock and folder changes (editor-only state, never saved with the level; currentEditMark skips them).
        struct OutlinerMetaChange { EditId id = 0; bool lockBefore = false, lockAfter = false; std::string folderBefore, folderAfter; };
        std::vector<OutlinerMetaChange> outlinerMeta;
        std::vector<std::string> foldersBefore, foldersAfter;   // the whole folder list, when foldersChanged
        editor::dl::Edit dataLayers;   // DATALAYERS: the layer list and membership change, replayed through applyDataLayerEdit()
        bool foldersChanged = false;
#if AVER_MODULE_SCENE
        editor::EntitySnapshot snap;    // scene entity: asset name, persisted id, every other component
#endif
        // Whether to give recreated/pasted entity a body (shape is derived fresh each time).
        bool hadBody = false;
        // Authoring flags (nocollide, snapToGround) not in snap; held in entityCollide_/entitySnapZ_. Fixes destroy-undo bug.
        bool  hadCollide = true;      // the default saveLevel writes for an entity it has no entry for
        bool  hadSnapZ   = false;
        f32   snapZ      = 0.0f;
        // Non-empty className: Create/Destroy of a graph-class actor, replayed by respawning the class (classSlot indexes
        // classAuthored_), not rebuilt from `snap`.
        std::string className;
        i32 classSlot = -1;
        // Object animation (empty clip = none); not in snap (Play advances its clock).
        EntityAnim hadAnim;
        // Vehicle preset (empty = no car); not in snap, level-held.
        std::string hadVehicle;
        MeshObj objSnapshot{};    // CreateObj/DestroyObj payload; MeshObj is trivially copyable
#if AVER_MODULE_PBR
        // Material payload: whole MaterialDesc (controls interact; replaying one field could restore invalid state). Undo depth bounds cost.
        pbr::MaterialHandle matHandle = 0;
        pbr::MaterialDesc matBefore{}, matAfter{};
#endif
        // Rename payload (display name, not CName).
        std::string renameBefore, renameAfter;

#if AVER_MODULE_SCENE
        // Prefab payload: the prefab state before and after one operation (create, place, apply, revert,
        // unpack, ...). Shared so copying a command around the undo stacks stays cheap.
        std::shared_ptr<editor::PrefabEdit> prefab;
        // [wave3/level-instances] Level instance payload: the instance state before and after one operation.
        std::shared_ptr<editor::LevelInstanceEdit> levelInst;
#endif

#if AVER_MODULE_SCENE
        // Removed component type and byte-exact contents (EntitySnapshot::Comp shape).
        editor::EntitySnapshot::Comp removedComponent;
        // PlacedComponent: one Add Component / Remove Component / Details edit of an animation or AI component, as the
        // COMPONENT record before and after (nullopt = the component is absent). One command covers Add, Edit and Remove.
        std::string pcType;
        std::optional<fmt::OcPlacedComponent> pcBefore, pcAfter;
#endif

        // LandscapeStroke: rect diff, not snapshot (512x512 section = 1 MB, touched rect = KB). One entry per stroke: one rect per
        // section the stroke touched (inclusive sample bounds, before / after).
        std::vector<game::GameLandscape::HeightEdit> landHeights;

        // wave3 LandscapeWeights: per touched section, its stored paint planes over a rect, plane-major, before / after.
        std::vector<game::GameLandscape::WeightEdit> landWeights;
        // wave3 WaterBodies: the level's RIVER / LAKE records before and after one edit gesture (a few records, so whole copies).
        std::vector<fmt::OcRiver> riversBefore, riversAfter;
        std::vector<fmt::OcLake> lakesBefore, lakesAfter;

        // FoliageStroke: every entity one stroke created/erased as one entry (dozens/sec would cost many Ctrl+Z). recreateFrom() undoes erases.
#if AVER_MODULE_SCENE
        std::vector<editor::EntitySnapshot> batchSnaps;
        std::vector<EditId>                 batchIds;
#endif
        bool batchWasErase = false;   // which direction undo has to run

#if AVER_MODULE_SCENE
        // Descendants (parents before children), fixing delete-subtree data loss. parent indexes THIS vector (-1 = root).
        struct DestroyedNode {
            EditId id = 0;
            i32 parent = -1;
            EditXform xf{};
            std::string label;
            editor::EntitySnapshot snap;
            bool hadBody = false;   // whether it had a body
            // Not components, so not in snap.
            bool  hadCollide = true;
            bool  hadSnapZ   = false;
            f32   snapZ      = 0.0f;
            EntityAnim hadAnim;
        };
        std::vector<DestroyedNode> subtree;

        // The destroyed entity's own parent, as an EditId (0 = was a root).
        EditId parentId = 0;

        // Reparent before/after parent (0 = root). Separate from parentId to avoid confusion.
        EditId reparentOldParentId = 0;
        EditId reparentNewParentId = 0;
#endif
    };

    EditId editIdFor(AvId e);
    AvId entityForEdit(EditId id) const;
    void rebindEdit(EditId id, AvId e);

    // Pushes a command onto the undo stack and clears the redo stack.
#if AVER_MODULE_PBR
    void applyMaterialDesc(pbr::MaterialHandle h, const pbr::MaterialDesc& want);
#endif

#if AVER_MODULE_SCENE
    void applyEntityLabel(EditId id, const std::string& label);

    void renameEntity(scene::Entity e, const std::string& to);

    // RemoveComponent's two apply halves: restoreComponent is undo's (puts captured bytes back);
    // removeComponentRaw is redo's, also called by removeComponentFromSelection before pushing undo.
    void restoreComponent(EditId id, const editor::EntitySnapshot::Comp& comp);
    void removeComponentRaw(EditId id, u32 type);

    // Removes one component from the selected entity as one undoable command -- the Details
    // panel's Remove Component handler. False when the entity is gone or does not carry `type`,
    // matching World::removeComponent's own refusal so a stale click is a silent no-op.
    bool removeComponentFromSelection(u32 type);

    // Visibility's apply: `undoing` picks which side of each VisibilityChange to write (mirrors
    // applyXformTo's `undoing`) -- one checkbox click is one gesture, so Ctrl+Z undoes all or none.
    void applyVisibilityTo(const EditCmd& c, bool undoing);

    // Collision's apply (same `undoing` rule), and the one write path for the flag: records it in
    // entityCollide_ (which saveLevel writes as `nocollide`) and drops or remakes the static body
    // through rebuildEntityBody, which itself honours the flag.
    void applyCollideTo(const EditCmd& c, bool undoing);
    void setEntityCollide(scene::Entity e, bool collide);

    // ---- object animation (a transform clip a placed mesh plays in Play) ----
    // writeEntityAnim records `a` in entityAnim_ and makes the entity's CAnimator match (an empty clip
    // erases both); applyEntityAnim adds the body's static/kinematic switch, which is the whole of an
    // undo/redo replay. setEntityAnim is the public one: apply + level dirty, NOT an undo entry -- the
    // Details panel pushes Kind::Animation itself. Refused for an entity with a CSkeletalMesh, whose
    // CAnimator is a skeletal clock. entityAnim returns nullptr when the entity has none.
    void writeEntityAnim(scene::Entity e, const EntityAnim& a);
    void applyEntityAnim(scene::Entity e, const EntityAnim& a);
    void applyAnimationTo(const EditCmd& c, bool undoing);
    void setEntityAnim(scene::Entity e, const EntityAnim& a);
    const EntityAnim* entityAnim(scene::Entity e) const;
    // Stop: every EntityAnim back onto its CAnimator, and each kinematic body put back at its restored
    // transform. Play: the animated entities' kinematic bodies follow what the anim tick just moved.
    void restoreAnimatedEntities();
#if AVER_MODULE_PHYSICS
    void driveAnimatedBodies(f32 dt);
#endif
#endif

    void pushEdit(EditCmd c);

    // The set the multi-move loop actually touches: selected, valid, not the anchor, not beneath
    // another selected entity. Skip rule DUPLICATED FROM THAT LOOP on purpose and must stay identical --
    // recording a before-state for a skipped entity, or missing one touched, is what this exists to
    // avoid. Sharing one helper keeps the two in step.
    template <class F>
    void forEachMultiMoved(F&& fn) {
#if AVER_MODULE_SCENE
        if (multiStale()) return;
        scene::World& w = scene::World::instance();
        for (const scene::Entity ent : selectionUi_.multiSel_) {
            if (ent == selectionUi_.selEntity_ || !w.valid(ent)) continue;
            bool ancestorSelected = false;
            for (scene::Entity p = w.parent(ent); p != scene::kInvalidEntity; p = w.parent(p))
                if (multiIsSelected(p)) { ancestorSelected = true; break; }
            if (ancestorSelected) continue;
            const auto* loc = w.component<scene::CLocal>(ent, scene::kComponentLocal);
            if (!loc) continue;
            fn(ent, loc->xf);
        }
#else
        (void)fn;
#endif
    }

    bool beginTransformEdit();
    void endTransformEdit();
    static bool nearlySameXform(const EditXform& a, const EditXform& b);

    static bool nearlySameTransform(const Transform& a, const Transform& b);

    void applyXformTo(const EditCmd& c, const EditXform& x, bool undoing = true);

#if AVER_MODULE_SCENE
    EditCmd describeEntity(scene::Entity e);

    scene::Entity spawnEntityFrom(const editor::EntitySnapshot& snap, const EditXform& xf,
                                   const std::string& label, bool hadBody,
                                   bool restoreObjectId = true,
                                   scene::Entity parent = scene::kInvalidEntity,
                                   bool collide = true, bool hasSnapZ = false, f32 snapZ = 0.0f,
                                   const EntityAnim* authoredAnim = nullptr);

    void recreateFrom(const EditCmd& c);

#if AVER_MODULE_SCENE
    void spawnSubtreeUnder(scene::Entity root, const std::vector<EditCmd::DestroyedNode>& subtree,
                           bool restoreIds);
#endif

#if AVER_MODULE_SCENE
    // ---- the World Outliner's reparent, as an undoable command ----------------------------

    static bool outlinerIsAncestorOf(const scene::World& w, scene::Entity maybeAncestor, scene::Entity e);

    bool isLevelOwned(scene::Entity e) const;

    enum class ReparentLegality { Ok, SelfOrDescendant, OffLevel };

    ReparentLegality reparentLegality(scene::Entity dragged, scene::Entity newParent) const;

    void pushReparent(scene::Entity child, scene::Entity newParent);

    void applyReparentTo(const EditCmd& c, EditId parentEditId, const EditXform& xf);

    void captureSubtree(EditCmd& c, scene::Entity e);
#endif

#if AVER_MODULE_PHYSICS
    // A body is a FUNCTION OF THE MESH AND CURRENT TRANSFORM, not carried through undo/redo/copy/
    // paste (see EditCmd::hadBody). THE ONE PLACE A BODY IS MADE: drops whatever `e` already owns in
    // entityBodies_ unconditionally (covers a replay against a since-destroyed handle, or running
    // before aver_phys_init), then, if physics is up, `e` is live and entityCollide_ does not say
    // nocollide, fits a fresh one from
    // CMeshRenderer::mesh and the CURRENT world transform (kinematic when entityAnim_ names it, else
    // static). TRIANGLES FIRST via
    // content_.collisionMeshFor/addStaticMeshBody; only a mesh with no cached collision mesh (a
    // built-in, or one whose .ocmesh failed to load) falls back to content_.boundsFor/addStaticBoxBody
    // (unit cube placeholder if even that's unknown).
    void rebuildEntityBody(scene::Entity e);

    // rebuildEntityBody for `e` and every descendant that owns a body. A descendant's LOCAL
    // transform doesn't change when an ancestor moves, but a body fits in WORLD space, which did.
    void rebuildMovedBodies(scene::Entity e);
#endif

    void destroyEntity(scene::Entity e);

    static void collectSubtree(const scene::World& w, scene::Entity e,
                               std::vector<scene::Entity>& out);
#endif  // AVER_MODULE_SCENE

    u64 currentEditMark() const;

    bool canUndo() const;
    bool canRedo() const;

    void objectsErasedAt(int index);
    void objectsInsertedAt(int index);

#if AVER_MODULE_LANDSCAPE && AVER_MODULE_SCENE
    void foliageBatchDestroy(const EditCmd& c);

    void foliageBatchRecreate(EditCmd& c);
#endif

#if AVER_MODULE_LANDSCAPE
    void generateLandscapeNoise(Engine& e);

    void beginSculptStroke();

    void endSculptStroke();

    void refreshFoliagePalette();

    static f32 foliageRand(u32& state);

#if AVER_MODULE_SCENE && AVER_WITH_IMGUI
    void handleFoliage(Engine& e, const ImGuiIO& io, bool overScene, f32 mx, f32 my);

    void foliagePlaceOne(Engine& e, f32 cx, f32 cy);

    void foliageErase(f32 cx, f32 cy);

    EditCmd foliageBatch_{};
    using FoliagePlaced = LandscapeToolState::FoliagePlaced;
#endif
#endif

    void undo();

    void redo();

    std::string selectionLabel() const;

    f32 selectedRadius() const;

    // --no-editor-chrome: suppress editor chrome for comparison with AverEngineRuntime.exe.
    bool noEditorChrome_ = false;

#if AVER_MODULE_SCENE
    scene::Entity playerStart_ = scene::kInvalidEntity;

    using AutosaveState = AutosaveSession::AutosaveState;



    void beginOutlinerRename(scene::Entity e);

#endif
    // OUTSIDE THE SCENE BLOCK IT WAS DECLARED IN, because its two callers are not in one. Both are
    // RAII guards in SandboxSelection.cpp -- undo and redo -- whose whole job is "however this body
    // returns, the Player Start handle is re-read afterwards", and neither body is guarded. The
    // definition keeps its own `#if AVER_MODULE_SCENE` around the part that needs a world, so with
    // no scene this is a function that still exists and does nothing, which is a far smaller thing
    // to get right than a destructor that exists in one configuration and not another.
    void refreshPlayerStart();

    // THE OPEN LEVEL'S FILE PATH AND DISPLAY NAME, outside the scene block they were declared in.
    // Both are plain strings about a file on disk. loadNavForLevel reads levelPath_ to find the
    // .ocnav baked beside it, and that function is guarded on AVER_MODULE_SYNAPSE -- the grid math,
    // which exists in a tree with no entity world at all. A path is not scene state even when
    // everything that path loads INTO is.
    std::string levelPath_, levelName_;

    f32 playerStartYaw_ = 0.0f;

    bool playerStartTransform(Vec3& outPos, f32& outYawDeg) const;

#if AVER_MODULE_SCENE
    scene::Entity makePlayerStart(const Vec3& at, f32 yawDeg);
#endif

    void addPlayerStart(Engine&);
#if AVER_MODULE_SCENE
    // "Add > Point/Spot/Rect Light": a CLight entity in front of the camera, selected, with an undo entry.
    void spawnLightAtCamera(i32 kind);
    // "Add > Decal": a CDecal entity in front of the camera, selected, with an undo entry.
    void spawnDecalAtCamera();
#endif

    void spawnPrimitive(Engine& engine, const char* assetPath, const char* label);

    void spawnCube(Engine& engine);

    Vec3 spawnPoint() const { return rset_.spawnAtOverride_ ? *rset_.spawnAtOverride_ : viewState_.camPos_ + camForward() * kAddDistance; }
    // Spawns one palette kind through the same functions as the toolbar's Add menu (and so the same undo entry).
    void placeActorKind(Engine& e, const std::string& kind, const Vec3* dropAt, bool onSurface);
    void buildPlaceActorsPanel(Engine& e);
    void buildMessageLogPanel();
    void buildUndoHistoryPanel();

#if AVER_WITH_IMGUI
// Drag-and-drop from the Content Browser, hence UI-only: spawnFromAssetDrop's sole caller is the
// viewport's ImGui drop target; dropWorldPoint exists only to serve it. Both lean on viewportRay/lowerExt.
#if AVER_MODULE_SCENE
    Vec3 dropWorldPoint(f32 screenX, f32 screenY, bool* onSurface = nullptr) const;
    // The first surface under a screen point (triangles, terrain, placeholder boxes); false on a
    // miss. Where Play From Here stands the pawn. SandboxViewport.cpp.
    bool pickSurfacePoint(f32 screenX, f32 screenY, Vec3& out);


    void spawnFromAssetDrop(Engine& e, const std::string& full, f32 screenX, f32 screenY);
#endif  // AVER_WITH_IMGUI
#endif  // AVER_MODULE_SCENE

    f32 gizmoLen(const Vec3& origin) const;

    bool project(const Vec3& wp, f32& sx, f32& sy) const;
    static f32 distToSeg(f32 px, f32 py, f32 ax, f32 ay, f32 bx, f32 by);

    void gizmoBasis(const EditXform& o, Vec3 ax[3]) const;

    int pickAxis(const Vec3& origin, const Vec3 ax[3], f32 L, f32 mx, f32 my) const;

    void applyMove(EditXform& o, f32 dx, f32 dy);
    void applyScale(EditXform& o, f32 dx, f32 dy);
    void applyRotate(EditXform& o, f32 px, f32 py, f32 mx, f32 my);

    void handleManip(Engine& e);
#if AVER_WITH_IMGUI && AVER_MODULE_SCENE
    bool dragDecalAndRopeHandles(const ImGuiIO& io, bool overScene, f32 mx, f32 my);
#endif
#if AVER_WITH_IMGUI && AVER_MODULE_SCENE
    void updateMarquee(const ImGuiIO& io, f32 mx, f32 my);
#endif
#if AVER_WITH_IMGUI
    void handleEditVerbKeys(const ImGuiIO& io);
#endif
#if AVER_WITH_IMGUI
    void applyGizmoDrag(f32 mx, f32 my);
#endif

#if AVER_MODULE_SCENE
    // Viewport placement verbs from handleManip; drop selection to floor or snap to grid.
#if AVER_WITH_IMGUI
    void snapSelectionToFloor();
#endif
    // Move by snap step or 10 cm.
    void nudgeSelection(const Vec3& deltaCm);

    // Authored visibility (OcWorldPlacement::visible), distinct from session-only H-hide below.
    bool authoredVisible(scene::Entity e) const;
    // Sets the bit directly, clearing any temporary H-hide state.
    void setAuthoredVisible(scene::Entity e, bool v);

    // H hides selection; Shift+H hides others; Ctrl+H restores (session-only, not saved).
    void hideSelection();
    void isolateSelection();
    void unhideAll();
    // Union of selection's world bounds for framing and gizmo anchor.
    bool selectionBounds(Vec3& center, f32& radius) const;
#endif

#if AVER_MODULE_LANDSCAPE && AVER_WITH_IMGUI
    void handleSculpt(Engine& e, const ImGuiIO& io, bool overScene, f32 mx, f32 my);
#endif

    void drawGizmo(Engine& e);

#if AVER_MODULE_LANDSCAPE
    void drawSculptCursor(Engine& e);
#endif

#if AVER_WITH_IMGUI
    void deleteSelection();

    void copySelection();

    void pasteClipboard();

    void duplicateSelection();

    void runCbMoveTest(const std::string& root);

    void runNotifyTest();

    void runPrefsWriteTest();

    void runProjectSwitchTest();

    void runGraphPrintTest();

    void runClearShaderCache();

    void runValidateGraph();

    void runRenameRepointTest();

    void runFindRefs();

    void runSaveDirtyTest();

    void runMultiSelectTest(Engine& eng);

    void runGraphHitsTest();

    void runAssetAssignTest();

    void runUndoTest(Engine& eng);
    void runClassPlaceTest(Engine& eng);

    void runKeybindPersistTest(const std::string& mode);

    void viewportRay(f32 screenX, f32 screenY, Vec3& ro, Vec3& rd) const;

#if AVER_MODULE_SCENE
    const aver::editor::PickGeometry& pickGeometryFor(u64 meshId);
#endif

    // Returns true when click hit something; false on Ctrl-click miss (leaves selection unchanged).
    bool pick(Engine& e, const ImGuiIO& io);

    bool dropButton(const char* label);

#endif

    void setUpgradeStatus(std::string msg,
                          editor::NotifySeverity sev = editor::NotifySeverity::Success);

    static bool onCloseGuardThunk(void* user);
    bool onCloseGuard();

    bool levelHasUnsavedEdits() const;

    void markLevelSaved();
    void markLevelUnsaved();

    // Serial the level was last written at. 0 means "as loaded, unedited".
    u64 savedEditMark_ = 0;
    u64 editSerialNext_ = 0;

    void requestExitChecked(Engine& e);

    void drawAboutPrompt(Engine& e);

    void drawSaveLevelAsPrompt();

    void drawExitPrompt(Engine& e);

    void openLevelPickerNow();

    void drawOpenLevelPrompt(Engine& e);

    void drawPendingOpenPrompt(Engine& e);

    // File > Launch in Aver Engine Runtime: starts AverEngineRuntime.exe on the open level.
    void launchInRuntime(Engine& e, bool skipDirtyCheck = false);

    void drawLaunchRuntimePrompt(Engine& e);

    void drawUpgradePrompt();

    // Outside #if AVER_WITH_IMGUI; body only calls setPrefBool/setPrefFloat/setPrefInt.
    void saveEditorPreferences();

    void buildUI(Engine& e);
#if AVER_WITH_IMGUI
    void drawFileMenu(Engine& e);
#endif
#if AVER_WITH_IMGUI
    void drawEditMenu();
#endif
#if AVER_WITH_IMGUI
    void drawWindowMenu();
#endif
#if AVER_WITH_IMGUI
    void drawBuildMenu(Engine& e);
#endif
#if AVER_WITH_IMGUI
    void drawMainToolbar(Engine& e, const ImVec2& wpos, const ImVec2& wsize, f32 toolbarH);
#endif
#if AVER_WITH_IMGUI
    void drawDockHost(const ImVec2& wpos, const ImVec2& wsize, f32 toolbarH, f32 statusH);
#endif
#if AVER_WITH_IMGUI
    void drawLevelTab(Engine& e);
#endif
#if AVER_WITH_IMGUI && AVER_MODULE_SCENE
    bool applySelectRequest();
#endif
#if AVER_WITH_IMGUI
    void applyOpenAssetRequests();
#endif
#if AVER_WITH_IMGUI
    void drawStatusBar(Engine& e, const ImVec2& wpos, const ImVec2& wsize, f32 statusH);
#endif

#if AVER_WITH_IMGUI
    // ---- Revision control: editor-facing half of RevisionControl.hpp ----
    // No worker processes spawned here; revisionControlRefresh/Select hand queries to workers.
    void revisionControlTick();
    // `force`: Refresh button, retries even when last answer was "no git here".
    void revisionControlRefresh(bool force);
    // Starts the history + diff query for one repo-relative path.
    void revisionControlSelect(const std::string& repoRelativePath);
    void buildRevisionControlPanel();

    // ---- Bottom status bar right-hand widgets ----
    // One helper icon+word, tinted by state; returns whether clicked.
    bool statusBarWidget(const char* id, const char* face, const ImVec4& tint, const char* tooltip);

    // Colour for revision control status based on editor::summariseForStatusBar.
    void drawRevisionControlStatusWidget();
    static ImVec4 rcMoodColour(editor::RepoMood m);

#if AVER_MODULE_MCP
    // Control-channel widget: loopback listener status and manual start/stop.
    void drawMcpStatusWidget();
#endif
    // Repo-relative path with forward slashes; empty if outside repository.
    std::string rcKeyFor(const std::string& absolute) const;
    // File status for Content Browser marking.
    bool rcMarkFor(const std::string& absolute, bool isDir, editor::FileStatus& out) const;
    // Shared badge colour across gallery, list, and file table.
    static ImU32 rcStatusColour(editor::FileStatus s);

    void drawGraphPrintOverlay(ImVec2 vpMin, ImVec2 vpMax);

    static void logLineStyle(LogLevel l, ImVec4& text, ImVec4& row, bool& filled);

    static void drawLogLine(LogLevel level, const char* text);

    void drawOutputLog();

    static std::string elide(const std::string& s, std::size_t maxLen);

    using ConsoleSuggestion = DrawerState::ConsoleSuggestion;

    std::vector<ConsoleSuggestion> computeConsoleSuggestions() const;

    void seedConsoleInput(const std::string& line);

    void acceptConsoleSuggestion(const std::string& insertText);

    void drawConsoleTranscriptLine(LogLevel level, const std::string& text);

    void drawConsoleVarRow(const editor::ConsoleVar& v);

    void drawConsoleTranscriptTab(Engine& e);

    void drawConsoleBrowserTab();

    void drawConsole(Engine& e);

    void runConsoleLine(Engine& e, const std::string& line);

    static int consoleInputCallback(ImGuiInputTextCallbackData* data);

    using CbRoot = DrawerState::CbRoot;
    std::vector<CbRoot> cbRoots() const;

    void cbNavigate(const std::string& dir);

    bool cbCanBack()    const;
    bool cbCanForward() const;

    void cbBack();
    void cbForward();

    std::string cbParentDir() const;

    static bool cbIsSourceFile(const std::string& ext);

    const editor::IdeInfo& cbIde() const;

    void cbOpenEntry(const std::string& full, bool isDir);

    void cbInvalidate(const std::string& dir);

    bool cbIsEditable(const std::string& path) const;

    // ---- Creating new assets in the browser ----
    // Three formats share all except the bytes written (New X = pick free path, write, adopt).

    std::filesystem::path cbFreeAssetPath(const char* stem, const char* ext);

    void cbAdoptNewAsset(const std::filesystem::path& target, bool openEditor = true);

    // Builds a starter pbr::MaterialDesc; guarded like cbCreateParticleEffect.
#if AVER_MODULE_PBR
    void cbCreateMaterial();
#endif

    void cbCreateSoundGraph();

    // Writes a starter .ocparticle and opens it.
#if AVER_MODULE_PARTICLES
    void cbCreateParticleEffect();
    void cbCreateParticleFromTemplate(usize index);   // a starter from ParticleTemplates.hpp
#endif

    void cbCreateFoliageType();

#if AVER_MODULE_VEHICLE
    void cbCreateVehicleAsset();   // a starter .ocvehicle (the template's sedan), opened in the Vehicle editor
#endif

    void cbCreateNodeGraph();

    void cbCreateBehaviourTree();
    void cbCreateStateTree();      // AI: wave 3 #21
    void cbCreateEqsQuery();
    void cbCreateSmartObject();
    void cbCreateUiLayout();
    void cbCreateFontSet();
    void cbCreateBlendSpace();
    void cbCreateStateMachine();
    void cbCreateMontage();
    // [wave5/ik-retarget] A starter .ocikrig (chains suggested from the skeleton's bone names when one is given), a
    // starter .ocretarget from a source skeleton (target left for the tab), and a starter .ocrig. With a skeleton path
    // the asset is created beside it; without, in the selected folder.
    void cbCreateIkRig(const std::string& skeletonPath = {});

    void cbCreatePhysAnim();   // a starter .ocphysanim
    void cbCreateFaceRig();   // [wave9/morph] a starter .ocface
    void cbCreateRetarget(const std::string& sourceSkeletonPath = {});
    void cbCreateRig();
    bool cbOpenRetargetFor(const std::string& targetSkeletonPath);
    void cbCreateMotionDb();   // a starter .ocmotion (motion matching database)
    void cbCreateSequence();   // [wave5/sequencer] a starter .ocseq

    // Writes a starter .ocinput (Input Scheme) and opens it.
    void cbCreateInputScheme();
    void cbCreateCaptions();   // a starter .occaptions (Content Browser > New Captions), CaptionEditor.hpp's captionStarterData

    // Writes a starter .ocgas (Gameplay Definitions) and opens it.
    void cbCreateGameplayDefs();

    // Writes a starter data table (.csv) / an empty string table for the default culture, and opens it.
    void cbCreateDataTable();
    void cbCreateStringTable(const std::string& name);

    std::string cbImportBlockedReason(const std::string& dir) const;

    std::string cbMoveDragPayloadFor(const std::string& dragged) const;

    static bool cbIsUnder(const std::string& path, const std::string& dir);

    void cbFolderDropTarget(const std::string& folderPath);

    std::vector<std::string> cbPruneNested(const std::vector<std::string>& in) const;

    bool cbCopyEntryTo(const std::string& src, const std::string& destDir);

    bool cbMoveEntryTo(const std::string& src, const std::string& destDir);

    std::vector<std::string> cbFindReferencesTo(const std::string& absPath) const;

    // ---- Repointing references after asset rename ----
    // Rewrites oldRel to newRel in every text asset; returns {files changed, references, failed}.
    // Cannot repoint hashes (fnv1a64 ids), non-text formats, or filenames (materialForSurface).
    struct RefRewriteReport { usize filesChanged = 0; usize refsRewritten = 0; usize filesFailed = 0; };

    RefRewriteReport cbRewriteReferences(const std::string& oldAbs, const std::string& newAbs);

    std::string cbRelativeToContent(const std::string& abs) const;

    void cbAfterMoveOrCopy(const std::vector<std::string>& srcs);

    void cbRenameEntry(const std::string& from, const std::string& newName, bool repointRefs = false);

    void cbRewriteHistory(const std::string& from, const std::string& to);

    void cbDuplicateEntry(const std::string& path);

    void cbDeleteEntry(const std::string& path);

    void cbCreateFolder(const std::string& parent, const std::string& name);

    void cbItemContextMenu(const std::string& full, const std::string& name, bool isDir);

    bool isEnginePath(const std::string& p) const;

    void drawContentBrowser();

    void cbFooter();

    void cbShortcuts();

    void cbFileOpModals();

    void drawBreadcrumb(const std::string& dir);

    editor::GraphAssetFamily graphAssetFamilyFor(const std::string& path);

    const DirListing& dirListing(const std::string& dir);
    // True when the content watcher covers `dir`, so its listing refreshes on watcher events.
    bool cbDirWatched(const std::string& dir) const;
    // Drops the cached listings the watcher's events this frame touched.
    void cbApplyWatchEvents();

    void drawFolderTree(const std::string& dir);

    using AssetKind = DrawerState::AssetKind;
    static const AssetKind* assetKindFor(const std::string& ext,
                                          editor::GraphAssetFamily graphFamily = editor::GraphAssetFamily::Gameplay);

    bool isSkinnedMeshEntry(const DirEntry& e) const;

    static ImU32 cardAccent(const std::string& ext, bool isDir, bool skinned,
                             editor::GraphAssetFamily graphFamily = editor::GraphAssetFamily::Gameplay);

    static int assetIconTile(const std::string& ext);

    int fileIconTile(const std::string& path, const std::string& name, const std::string& ext);

    static void folderGlyph(ImDrawList* dl, ImVec2 c, f32 s, ImU32 col);

    static void fileGlyph(ImDrawList* dl, ImVec2 c, f32 s, ImU32 col);

    static void blitTile(ImDrawList* dl, u64 tex, ImVec2 centre, f32 s, f32 aspect, int tile, int tiles);

    void typedGlyph(ImDrawList* dl, ImVec2 c, f32 s, const AssetKind& k);

    void drawEntryIcon(ImDrawList* dl, ImVec2 centre, f32 s, bool isDir, int tile, bool module,
                       const std::string& kindExt = std::string(),
                       editor::GraphAssetFamily graphFamily = editor::GraphAssetFamily::Gameplay);

    static std::string lowerExt(const std::filesystem::path& p);

    static bool isPlaceableAssetExt(const std::string& ext);


    bool cbIsSelected(const std::string& path) const;

    void cbClickSelect(const std::vector<const DirEntry*>& shown, int index);

    void cbSelectAll(const std::vector<const DirEntry*>& shown);

    void cbClearSelection();

    static std::string cbDragLabel(const std::string& name, const std::string& blob);


    using AssetChoice = DrawerState::AssetChoice;

    bool assetPicker(const char* popupId, const std::vector<AssetChoice>& candidates,
                     u64 current, u64* picked);

    using PickerCands = DrawerState::PickerCands;
#if AVER_MODULE_SCENE

    // ---- Details > Add Component for the animation and AI components (SandboxComponentDetails.cpp) ----
    void drawPlacedComponentSections();                                   // called once from the Details panel
    bool addPlacedComponent(const editor::PlacedKind&);                   // create + defaults + one undo entry + markLevelUnsaved
    bool removePlacedComponentFromSelection(const editor::PlacedKind&);   // capture, remove (Brain: stop first), one undo entry
    void beginPlacedGesture(const editor::PlacedKind&);                   // captures the "before" of a drag / typed edit
    void commitPlacedEdit(const editor::PlacedKind&, std::optional<fmt::OcPlacedComponent> before);   // capture "after", push if different
    // true when changed. `accept` (optional) filters the picker's candidates, e.g. Animator drops object clips.
    bool drawAssetField(const char* id, const char* label, std::string& path, std::span<const char* const> exts,
                        const std::function<bool(u64 id, const std::string& absPath)>& accept = {});
    void drawPlacedAddMenu();                                             // the "Animation and AI" entries of the Add Component popup
    bool drawPlacedRemoveMenu();                                          // ...and of the Remove popup; true when any is present
    std::string pathOfContentId(u64) const;                               // cbRelativeToContent(content_.pathFor(id)), "" if not indexed
    // Undo / redo: puts one component into `st` (nullopt = absent) on the entity behind `id`.
    void applyPlacedComponentState(EditId id, const std::string& type, const std::optional<fmt::OcPlacedComponent>& st);
    editor::AttachContext placedAttachContext(scene::Entity e) const;
    std::optional<fmt::OcPlacedComponent> placedBefore_;                  // the open gesture's "before"
    const editor::PlacedKind* placedGesture_ = nullptr;                   // the kind that gesture edits; null = none open
    // Authored component state at Play start, so a save during Play writes it and not what a script did since.
    std::unordered_map<u32, std::vector<fmt::OcPlacedComponent>> placedBaseline_;
#endif

    // ---- Three assignments (each asset id type is different) ----
    // NOT UNDOABLE: asset writes mark level dirty with no EditCmd.
#if AVER_MODULE_SCENE
    bool assignParticleEffect(scene::Entity ent, const std::string& absPath);

    bool assignMeshId(scene::Entity ent, u64 meshId);

    bool assignMaterialToken(scene::Entity ent, i32 token);
#endif

    static bool containsNoCase(const std::string& hay, const char* needle);

    static std::string fitLabel(const std::string& name, f32 wrap, int lines);

    void drawFolderGallery(const std::vector<const DirEntry*>& shown);

    void cbGatherDeepMatches(const std::string& dir, std::vector<const DirEntry*>& out, int depth);

    void drawFolderFiles(std::string dir);

    void drawImportModal();

    void notifyOutcome(editor::NotifySeverity sev, std::string title, std::string body,
                      bool offerLog = false);

    std::string importDestLabel(const std::string& absDir) const;

    // overwrite: explicit choice in Import dialog to replace collision.
    void importAsset(const std::string& src, const std::string& destDir, bool overwrite = false);
    // FILES DROPPED ON THE WINDOW (Window::takePendingDroppedFiles). Imports into current folder.
    void importDroppedFiles(const std::vector<std::string>& paths);

#if AVER_HAVE_AUDIO_IMPORT
    void importAudio(const std::string& src, const std::string& destDir, bool overwrite = false);
#endif

    void importModel(const std::string& src, const std::string& destDir, bool overwrite = false);

    // Writes edited material back to .cs source under Content\Materials. Returns file written or "".
#if AVER_MODULE_PBR
    std::string saveMaterialSource(const std::string& name, const pbr::MaterialDesc& d, std::string& err);
#endif

#if AVER_MODULE_PBR
    void materialPanel(pbr::MaterialHandle handle);
#endif

    void buildModePanel(Engine& e);

    static bool editField(const char* label, std::string& value, usize cap);

    void buildSelectModePanel();

    void buildSimulateModePanel(Engine& e);

#if AVER_MODULE_SCENE
    // The Animate mode's panel and timeline window; the logic lives in seqEditor_.
    void buildAnimateModePanel();
    void buildSequencerWindow();
    editor::SequenceHost sequenceHost();
    // [wave5/sequencer] The frame's two sequence steps. Pre: before the animation graphs and AnimSystem::tick
    // (render-queue pose, clock, property / visibility / animation / event / audio tracks). Post: after the tick,
    // before World::flush (transform, emissive, camera).
    void tickSequencePre(Engine& e, f32 dt);
    void tickSequencePost();
    // Opens a .ocseq as the Sequencer's current sequence (Content Browser double-click).
    bool openSequenceFile(const std::string& fullPath);
    // The --sequence-play driver (SandboxSequenceRun.cpp): steps the run before the sequence ticks,
    // reads and writes the numbered captures in onRender, and ends the process with its verdict.
    void sequenceRunStep(Engine& e);
    void serviceSequenceCapture(Engine& e);
    bool sequenceRunActive() const { return seqRun_.phase != SequenceRunState::Phase::Off && seqRun_.phase != SequenceRunState::Phase::Done; }
    int  sequenceRunExit() const;
    // Render queue jobs: the per-frame driver and capture reader, the screen takeover (buildUI draws
    // nothing and the scene fills a window sized to the output) and the Window > Render Queue panel's host.
    void renderJobStep(Engine& e);
    void serviceRenderCapture(Engine& e);
    bool renderJobEnter(Engine& e);
    void renderJobFinish(Engine& e, bool ok);
    void renderJobRewind(i64 frame, i32 sub);
    bool renderJobOwnsScreen() const { return seqRun_.owns; }
    void applyRenderJobViewport();
    editor::RenderQueueHost renderQueueHost();
    // The Animate mode's camera path as a line mesh, rebuilt when the sequence or the playhead moves.
    void drawSequencePath(Engine& e);
#endif

    // Water is not a terrain feature (buildWaterPanel touches levelHeader_.waters, not landscape types).
    void buildWaterPanel(Engine& e);

#if AVER_MODULE_LANDSCAPE
    void buildLandscapeModePanel(Engine& e);

    void foliagePanelCreateType();

    void buildFoliageModePanel();
#endif

    void markLevelRecordEdited(bool& has);

    void buildPanels(Engine& e);

#if AVER_MODULE_SCENE
    using OutlinerRow = SelectionState::OutlinerRow;

    static std::string lowerCopy(std::string v);

    // What an Outliner row is, for its icon and the type filter; the icon that type leads its row with.
    editor::OutlinerType outlinerTypeFor(scene::Entity ent) const;
    const char* outlinerIconFor(scene::Entity ent) const;

    bool entityLocked(scene::Entity e) const { return !selectionUi_.outlinerLocked_.empty() && selectionUi_.outlinerLocked_.count(static_cast<u32>(e)) != 0; }
    std::string outlinerFolderOf(scene::Entity e) const;
    void applyOutlinerMeta(const EditCmd& c, bool undoing);
    // Folders and locks ride in the .ocworld (OUTFOLDER / OUTITEM); placements and lights are named by slot.
    void captureOutlinerMeta(fmt::OcWorldData& w, const std::unordered_map<u32, i32>& placementSlotOf,
                             const std::unordered_map<u32, i32>& lightSlotOf, bool streamed);
    void restoreOutlinerMeta(const fmt::OcWorldData& w, const world::LevelInstance& inst, const std::vector<scene::Entity>& lights);
    void clearOutlinerMeta();
    void outlinerSetLocked(const std::vector<scene::Entity>& ents, bool lock);
    void outlinerSetVisible(const std::vector<scene::Entity>& ents, bool visible);
    void outlinerMoveToFolder(const std::vector<scene::Entity>& ents, const std::string& folder);
    std::string outlinerNewFolder(const std::string& parent, const std::vector<scene::Entity>& members);
    void outlinerRenameFolder(const std::string& from, const std::string& to);
    void outlinerDeleteFolder(const std::string& path);
    void drawOutlinerFilterBar();
    void drawOutlinerFolders(const std::string& parent);
    void drawOutlinerRowIcons(scene::Entity ent, float y0, float y1, bool selected, bool rowHovered);
    float outlinerIconZone() const;

    std::string outlinerLabelFor(scene::Entity e) const;

    void drawOutlinerDragSource(scene::Entity ent);

    void drawOutlinerDropTarget(scene::Entity ent);

    // recordOrder false: caller already pushed this row to outlinerOrder_.
    void drawOutlinerRow(const OutlinerRow& row,
                         const std::unordered_map<u32, std::vector<const OutlinerRow*>>& children,
                         int depth, bool recordOrder = true);

    // Flat cache: only rows on screen (ImGuiListClipper).
    void drawOutlinerFlatRows();
    // Tree cache: roots with children, then childless roots on-screen.
    void drawOutlinerTreeRows();

    // Cache staleness detection: outlinerSignature (O(1) stamp) and per-slot fingerprints.
    u64  outlinerSignature() const;
    u64  outlinerEditMark() const;
    u64  outlinerSlotSignature(u32 slot) const;
    bool outlinerAuditDiverged(bool full);
    // Rebuilds when cache is stale; no-op otherwise.
    void refreshOutlinerCache();
    // Rebuilds outlinerRows_/outlinerChildren_/outlinerRoots_.
    void rebuildOutlinerCache();
    bool outlinerPlaying() const;
#endif

    void buildOutlinerPanel();

    void buildDetailsPanel(Engine& e);
#if AVER_MODULE_SCENE
    void drawEntityNameAndClass(const std::string& nm);
#endif
#if AVER_MODULE_SCENE
    void drawEntityPrefabSection(const std::string& nm, const std::vector<scene::Entity>& multiSelected);
#endif
#if AVER_MODULE_SCENE
    void drawEntityTransformSection(scene::World& w, const std::vector<scene::Entity>& multiSelected);
#endif
#if AVER_MODULE_SCENE
    void drawMeshVisibleCollides(scene::World& w, const std::vector<scene::Entity>& multiSelected);
#endif
#if AVER_MODULE_SCENE
    void drawMeshAnimation(scene::World& w, const std::vector<scene::Entity>& multiSelected);
#endif
#if AVER_MODULE_SCENE
    void drawMeshAssetPicker(scene::CMeshRenderer* mr, const std::vector<scene::Entity>& multiSelected);
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_PBR
    void drawMeshMaterialSection(scene::CMeshRenderer* mr, const std::vector<scene::Entity>& multiSelected);
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_PARTICLES
    void drawParticleEmitterDetails(scene::World& w);
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS
    void drawSoftBodyDetails(scene::World& w);
#endif
#if AVER_MODULE_SCENE
    void drawAddRemoveComponent(scene::World& w);
#endif
    void drawSunDetails();
    void drawSkyDetails(Engine& e);
    void drawPostProcessDetails(Engine& e);

    void drawDrawer(Engine& e);

    void toggleDrawer(Drawer d);

    static ImVec4 notifyColour(editor::NotifySeverity s);

    void drawNotifications();

    void loadEditorPreferences();

    void resolvePreferredIdeFromPrefs();


    void buildEditorPrefs();
    // The centred Play group of the main toolbar.
    void drawPlayToolbar(Engine& e);
    // Editor Preferences > Play page.
    void buildPlayPrefsSection();

    void buildWorldSettings();

    void buildProjectSettings();

#if AVER_MODULE_VOXI
    static void featureStatusBadge(aver::voxi::Renderer& vx, aver::voxi::Feature f);
#endif

    // Window/Import/Streaming/Physics/Audio are not render pages but follow AVER_MODULE_VOXI guard.
    bool settingInt(const char* label, int* v, int lo, int hi, int whenEnabled, const char* tip);

    void buildWindowSettings();

    void buildImportSettings();

    void buildStreamSettings();

    void buildPhysicsSettings();

    void buildAudioSettings();

#if AVER_MODULE_VOXI
    void buildRenderingSettings(int page);
#if AVER_WITH_IMGUI && AVER_MODULE_VOXI
    void renderPageOverview(voxi::Renderer& vx, voxi::Settings& s, const voxi::Resolution& er, bool& changed, u32& overallFollowMask);
#endif
#if AVER_WITH_IMGUI && AVER_MODULE_VOXI
    void renderPageAntiAliasing(voxi::Renderer& vx, voxi::Settings& s, const voxi::Resolution& er, bool& changed);
#endif
#if AVER_WITH_IMGUI && AVER_MODULE_VOXI
    void renderPageGi(voxi::Renderer& vx, voxi::Settings& s, const voxi::Resolution& er, bool& changed);
#endif
#if AVER_WITH_IMGUI && AVER_MODULE_VOXI
    void renderPageRayTracing(voxi::Renderer& vx, voxi::Settings& s, const voxi::Resolution& er, bool& changed);
#endif
#if AVER_WITH_IMGUI && AVER_MODULE_VOXI
    void renderPageDenoising(voxi::Renderer& vx, voxi::Settings& s, const voxi::Resolution& er, bool& changed);
#endif
#if AVER_WITH_IMGUI && AVER_MODULE_VOXI
    void renderPageMaterials(voxi::Renderer& vx, voxi::Settings& s, const voxi::Resolution& er, bool& changed);
#endif
#if AVER_WITH_IMGUI && AVER_MODULE_VOXI
    void renderPagePost(voxi::Renderer& vx, voxi::Settings& s, const voxi::Resolution& er, bool& changed);
#endif
#if AVER_WITH_IMGUI && AVER_MODULE_VOXI
    void renderPagePerformance(voxi::Renderer& vx, voxi::Settings& s, const voxi::Resolution& er, bool& changed);
#endif
#if AVER_WITH_IMGUI && AVER_MODULE_VOXI
    void renderPageDebug(voxi::Renderer& vx, voxi::Settings& s, const voxi::Resolution& er, bool& changed);
#endif
#endif

    void buildViewportOverlay();
    void drawViewportViewTypeMenu();
    void drawViewportViewModeMenu();
    f32 drawViewportShowMenu();
    void drawViewportModeCombo(f32 gap);
    void drawViewportLandscapeTools(f32 icon, f32 gap);
    void drawViewportTransformTools(f32 icon, f32 gap, f32 tinyGap);
    f32 drawViewportCameraSpeed(f32 gap);
    // Viewport right-click menu (Play From Here). RMB armed on press, decided on release.
    void drawViewportContextMenu();
    using ViewportCtx = ViewportState::ViewportCtx;
#endif

    // Requests probe pixel and screenshot on capture run, then reports what was read.
#if AVER_MODULE_SYNAPSE
    void navBakeCheck(Engine& e);
#endif

    void rayProbeCheck(Engine& e);

    void gpuTimingCheck(Engine& e);

    void resizeCheck(Engine& e);

    void captureCheck(Engine& e);

    void lumaSweepCheck(Engine& e);

    u64 maxFrames_; bool headless_; std::string beamPath_, shot_;
    // Default to Move (gizmo ready on open, vs Select which needs a second key press).
    Tool initialTool_ = Tool::Move;
    std::vector<MeshObj> objects_;
    bool hideEditorScene_ = false;
    rhi::IDevice* prefsDevice_ = nullptr;
    std::string prefIdeName_;
    bool prefsLoaded_ = false;
    // Set by loadLevel when level supplied CAMERA record; read by onAttach.
    bool levelCameraRestored_ = false;
    editor::AssetEditorHost assetEditors_;
    bool wantMeshReload_ = false;
    // Outliner display names (not scene::World::name which holds asset path).
    std::unordered_map<u32, std::string> entityLabels_;
#if AVER_MODULE_SCENE
    editor::DecalImageResolver decalImages_;  // a decal's image id -> a file under the project's content folder
    editor::DecalGizmo decalGizmo_;           // the selected decal's box and its face handles

    // PREFABS: the library (loaded assets), the system (instances in the world), the editor's operations on
    // them and the Create Prefab dialog. The system's hooks keep levelEntities_, labels and bodies in step.
    prefab::PrefabLibrary prefabLib_;
    prefab::PrefabSystem prefabSys_{scene::World::instance(), prefabLib_};
    editor::PrefabEditorModel prefabModel_{scene::World::instance(), prefabLib_, prefabSys_};
    editor::PrefabCreateDialog prefabDlg_;
    void installPrefabHooks();
    editor::PrefabUiCallbacks prefabUiCallbacks();
    // One undo entry for a prefab operation.
    void pushPrefabEdit(const editor::PrefabEdit& edit);
#endif
    std::unordered_map<std::string, int> labelCounts_;
    std::unordered_map<u32, int32_t> entityBodies_;

    static constexpr std::size_t kUndoDepth = 128;
    std::vector<EditCmd> undoStack_, redoStack_;
    std::unordered_map<EditId, AvId> editToEntity_;
    std::unordered_map<u32, EditId> entityToEdit_;
    EditId nextEditId_ = 1;
    EditXform editBefore_{};
    bool editBeforeValid_ = false;
    // Non-anchor entities' local transforms at drag start (alsoMoved in EditCmd).
    std::vector<std::pair<EditId, Transform>> multiMoveBefore_;

    // Copy/Duplicate source for Paste; entities/hasObject set exclusively by copySelection.
    struct ClipboardEntity {
#if AVER_MODULE_SCENE
        editor::EntitySnapshot snap;
        // Everything under it too; copying parent alone used to paste childless.
        std::vector<EditCmd::DestroyedNode> subtree;
#endif
        EditXform xform{};
        bool hadBody = false;
        // Root's non-component flags and object animation (children keep theirs in subtree).
        bool  hadCollide = true;
        bool  hadSnapZ   = false;
        f32   snapZ      = 0.0f;
        EntityAnim hadAnim;
    };

    struct EditorClipboard {
        // Empty means nothing was copied.
        std::vector<ClipboardEntity> entities;

        // Placeholder path single-item (objects_/MeshObj has no multi-selection).
        bool hasObject = false;
        MeshObj object{};
    };
    EditorClipboard clipboard_;

#if AVER_MODULE_PBR
    // One material edit: desc before interaction and which material.
    bool matEditActive_ = false;
    bool matEditDirty_  = false;
    pbr::MaterialHandle matEditHandle_ = 0;
    pbr::MaterialDesc   matEditBefore_{};
#endif

    // Chord registry (defaults match historic hardcoded keys). See EditorKeybinds.hpp.
#if AVER_WITH_IMGUI
    // Reference to the one registry (Preferences rebinds reach tabs too via editor::keybinds()).
    editor::KeybindRegistry& keybinds_ = editor::keybinds();
#endif

    std::string makeEntityLabel(const std::string& surface, const std::string& asset);
    // Returns "Wood" for M_Wood, else the asset's stem, else "Entity"; lets saveLevel distinguish generated from renamed labels.
    static std::string entityLabelBase(const std::string& surface, const std::string& asset);

#if AVER_MODULE_LANDSCAPE

    using FoliageSpecies = LandscapeToolState::FoliageSpecies;

    using LandscapeTab = LandscapeToolState::LandscapeTab;
#if AVER_WITH_IMGUI
    void buildLandscapePaintTab(Engine& e);
    void handlePaint(Engine& e, const ImGuiIO& io, bool overScene, f32 mx, f32 my);
    void endPaintStroke(const char* label = "Paint terrain");
    void purgeWeightUndo();
#if AVER_MODULE_FLUIDS
    void buildLandscapeWaterTab(Engine& e);
    void handleWaterSpline(Engine& e, const ImGuiIO& io, bool overScene, f32 mx, f32 my);
    void waterSnapshotIfIdle();
    void waterEdited();                   // opens a gesture, marks the level unsaved, schedules the rebuild
    void waterGestureEnd(const char* label);
    void carveSelectedBody(Engine& e);
#endif
#if AVER_MODULE_SCENE
    void scatterFoliageFromLayer(Engine& e);
    void buildBiomeSection();   // LandscapeBiomeTab.cpp: Landscape > Paint, the biome list and Generate Vegetation
    // One instance of `sp` at (x, y), scale / yaw drawn from `rng` (foliagePlaceOne's tail, so the brush and the
    // layer scatter place identically).
    void foliagePlaceAt(Engine& e, const FoliageSpecies& sp, f32 x, f32 y, u32& rng);
#endif
#if AVER_MODULE_PBR
    bool buildTerrainLayersSection(pbr::MaterialDesc& d, const std::function<bool(bool)>& track);
#endif
#endif
#if AVER_MODULE_FLUIDS
    // Rebuilds stale bodies and draws the Water tab's chrome. Every frame, in any mode.
    void drawWaterBodies(Engine& e);
#endif
    // ---- end wave3 layers/rivers ----
#endif

    bool editorModeIsLandscape() const;

    bool editorModeAvailable(EditorMode m, const char** whyNot = nullptr) const;

    bool editorModeIsFoliage() const;

    void setEditorMode(EditorMode m);

    void toggleEditorMode();
    std::string startMode_;

    void applyStartMode();
    // The projection and owner-hide mode of the Play camera (GameCamera.hpp): FOV, roll, clip planes, the collision fade. The default outside Play.
    game::PlayView playView_;
    Vec3 viewPos() const { return viewState_.viewOverride_ ? viewState_.viewPosOv_ : viewState_.camPos_; }
    // Moves the editor camera without turning it; streaming treats it as a jump, not a fast move.
    void teleportCamera(const Vec3& p);
    Vec3 viewForward() const { return viewState_.viewOverride_ ? viewState_.viewFwdOv_ : camForward(); }

    // The pawn viewed in FIRST PERSON this frame, or kInvalidEntity. RESET UNCONDITIONALLY at the top of drivePlayCamera().
    // Used by owner-hide check: a mesh with kMeshRendererHiddenFromOwner skips rasterised draw when this is itself or an ancestor.
#if AVER_MODULE_SCENE
    scene::Entity firstPersonPawn_ = scene::kInvalidEntity;
#endif
    // The editor's colour-vision preview (View menu, editor prefs only) and this frame's accessibility options and captions.
    editor::AccessibilityPreview accessPreview_;
    game::AccessFrame access_;
    editor::PostVolumeTools postVolumeTools_;   // POSTVOLUME records: Add menu, boxes, Details, viewport blend
    bool showPostVolumes_ = true;               // Show > Post Volumes: every volume's box, not just the selected one's

    // Frame time and the Play-only CPU phases, fed by onUpdate's begin()/end() brackets and folded in the status bar.
    editor::PlayProfile playProf_;

#if AVER_WITH_IMGUI
    ImFont* fontMedium_=nullptr; // Roboto Medium, for the menu bar; null if only the fallback loaded
#endif
#if AVER_WITH_IMGUI_D3D12
    // Editor's Dear ImGui backend, installed into the device non-owning. Deleted late in onShutdown for device's uiShutdown().
    std::unique_ptr<rhi::d3d12::IUiBackend> uiBackend_;
#endif
#if AVER_WITH_IMGUI_VULKAN
    // The Vulkan backend, held on same terms as the D3D12 one above for teardown ordering.
    std::unique_ptr<rhi::vkb::IUiBackend> uiBackendVk_;
#endif

    // Registered once in onInit, unregistered in onShutdown. Never rebuilt: one instance for the whole run.
    GBufferDebugFeature gbufferDebugFeature_;
#if AVER_WITH_SYNAPSE_AI && AVER_MODULE_SYNAPSE_GPU
    // The GPU crowd-avoidance backend, installed behind the Synapse ABI's one SynapseAi (game::installCrowdGpu).
    synapse::GpuCrowdBackend crowdGpu_;
#endif
    // Window > Neural Visualiser's NeuraFI overlay. Registered beside gbufferDebugFeature_, for the run.
    NeuraFiVizFeature neurafiViz_;
// AND AVER_MODULE_SCENE, not just OCCLUSION -- scene::Entity keys on these members.
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
    bool occlusionWasVisible(scene::Entity e) const;
#endif
#if AVER_MODULE_SR
    // Why the CURRENTLY APPLIED level is what it is -- Auto/Manifest/User/Cli/ForcedOff.
    // FSR 1 (EASU + RCAS). Built whenever the scene is scaled (any AverSR level or a manual render
    // scale) or edge AA is on; dropped, after the device lets go of it, when none is (applyUpscalerSlot).
    std::unique_ptr<aver::sr::FsrUpscaler> averSrUpscaler_;
    // Temporal AA (TAAU + RCAS): when on, it takes the slot instead of FSR, at any render scale.
    std::unique_ptr<aver::sr::TemporalUpscaler> taaUpscaler_;
    // NeuRAA wraps whichever upscaler holds the slot. Phase 1: only while its debug view is selected.
    std::unique_ptr<aver::sr::NeuRaa> neuraa_;

    std::unique_ptr<aver::neurafi::NeuraFI> frameInterpolator_;
    // Decides and pushes this frame's frame-interperation state; returns whether it is wanted (the G-buffer must then be on).
    bool updateFrameInterpolation(aver::rhi::IDevice* dev);
#endif
    bool camOverride_=false;         // --cam X Y Z PITCH YAW: aim the viewport camera outright
    // --set NAME VALUE pairs, applied once through runConsoleLine on the first frame with a device.
    std::vector<std::pair<std::string, std::string>> consoleSets_;

    Vec3 camPosOverride_{};
    f32  pitchOverride_=0.0f, yawOverride_=0.0f;   // radians, converted in setCamera
    bool useWarp_=false;             // --warp: run on the D3D12 software rasteriser
    // The engine, for the log sink to write the startup splash through. Set at the top of onInit.
    Engine* engineForSplash_ = nullptr;
    std::thread::id mainThreadId_{};
    bool windowedOverride_=false;    // --windowed: kept so the flag still parses; windowed is the default now
    bool fullscreenOverride_=false;  // --fullscreen: opt an interactive run INTO borderless fullscreen
    std::string backendName_;   // --backend: which RHI backend to ask for first
    std::string cultureOverride_;   // --culture: beats the saved preview culture (editor pref loc.previewCulture)
    // RENDER.BACKEND as the OPEN PROJECT states it. Separate from backendName_ (what this run launched with).
    std::string projectBackend_;
    std::string runningBackend_ = "?";   // what the device actually came back as, latched at init
    bool debugLayer_=false;          // --debug-layer: validate every graphics call (a real per-call tax)
    std::string scriptsDir_;         // --scripts <dir>: where to look for user script assemblies
    editor::ProjectBrowser browser_;
    fmt::ProjectDesc project_;
    std::string projectPath_;        // <path>.ocproject given on the command line
    std::string openMapPath_;        // <path>.ocmap given on the command line, if any
    bool browserActive_=false;
    // Whether THIS launch's own command line was eligible to register as the single-instance primary.
    bool singleInstanceEligible_ = false;
    bool        armOpenLevelPicker_ = false;   // --open-level-picker, consumed on the first draw
    rhi::TextureHandle logoTexture_=0;   // 0 when logo.png was absent or undecodable
    int frameNo_ = 0;                                      // bumped once per UI frame; the cache freshness clock
    rhi::TextureHandle compileIconTexture_=0;   // the Compile C# status sprite sheet (3 tiles)
    u64 compileIconUiId_=0;
    u64 logoUiId_=0;
    rhi::TextureHandle logoSmallTexture_=0;   // the logo area-averaged down to the menu bar's size (the full one has no mips)
    u64 logoSmallUiId_=0;
    f32 logoAspect_=1.0f;
    editor::ToolsMenu tools_;
    editor::Nrd2Session nrd2Session_;   // Tools > Train Neural Denoiser, --nrd2-train
    editor::ShaderWarmup shaderWarmup_; // Sandbox.exe --warm-shaders after a project opens
    // A level's new material graphs compile in a --warm-shaders --graphs run while Voxi holds its rebuild (GraphShaderWarm.cpp).
    editor::ShaderWarmup graphWarmup_;
    u64 graphWarmRev_ = 0;               // the registry revision the running graph warm-up was started for
    bool graphWarmHeld_ = false;
    std::chrono::steady_clock::time_point graphWarmT0_{};
    void tickGraphShaderWarm(Engine& e);
    // "Preparing <level>" after the loading screen while ray-tracing structures still build (SandboxRender.cpp).
    bool levelPrepArmed_ = false;
    u64 levelPrepToast_ = 0;
    std::chrono::steady_clock::time_point levelPrepT0_{};
#if AVER_MODULE_VOXI
    voxi::VoxiRenderer voxiRenderer_;
    editor::SceneLightFeed sceneLightFeed_;   // CLight entities -> Voxi lamps + path-tracer lights, per frame
    game::SceneDecalFeed sceneDecalFeed_;     // CDecal entities -> Voxi's projected-decal list, per frame
    game::EnvFxFeed envFxFeed_;               // G4 R8: the framework's fog volumes -> Voxi's volumetric fog, per frame
#endif
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // Registration is non-owning: particleRenderer_ must outlive the device, torn down in onShutdown.
    particles::ParticleRenderer particleRenderer_;
#endif

    // Water is an INDEPENDENT module from Particles. --water <heightCm> is the opt-in.
#if AVER_MODULE_FLUIDS
    // The level's analytic surface and simulated volumes, shared with the runtime.
    game::GameWater water_;
#endif
#if AVER_MODULE_SCRIPTING
    void configureProjectInput(const char* when);   // the project's INPUT.SCHEME, at Play and level spawn (SandboxPlay.cpp)
    static i32 animCurve(i32 entity, i64 nameHash, f32* outValue, void*);

#if AVER_MODULE_SYNAPSE_SCENE
    static i32 synapseTarget(i32 entity, f32* outX, f32* outY, f32* outZ, void*);

#if AVER_MODULE_FRAMEWORK
    static scene::Entity synapseTargetResolver(void*);
#endif

    static i32 synapsePerception(i32 entity, i32* outCanSee, i32* outLastTarget,
                                 f32* outTimeSinceSeen, void*);
#endif

    // NEEDS THE SCENE AS WELL AS SCRIPTING: parameter is a scene::Entity, sink is aver::anim::AnimNotifyFn.
#if AVER_MODULE_SCENE
    static void animNotify(scene::Entity e, const char* name, void* user);
#endif

    scripting::ScriptHost scripts_;
    // The graph debugger and the Problems panel's before-Play pass, once a frame (SandboxApp.cpp).
    void pumpGraphDebugger();
    bool graphDiagPlayWas_ = false;
#endif
    // The retained game UI's renderer. Heap-owned because create() may decline.
    aver::render::ui::UiRenderer* gameUi_ = nullptr;
    std::unique_ptr<aver::render::SkinSelfTest> skinSelfTest_;
    bool skinDrawTest_ = false;   // --skin-draw-test: does the RASTERISER read the skinned buffer
    // BESIDE skinDraw_, outside the scene guard: ThumbnailCache (sandbox/src/ThumbnailCache.hpp) stands on the RHI and ActorPreview only.
    // The content browser already treats that as settled.
    aver::editor::ThumbnailCache thumbnails_;
    // G4 R5: render-to-texture views a Play session asks for. Idle (and allocating nothing) while the registry holds no view; the registry is cleared when Play ends.
    aver::game::SubViewportHost subViews_;
    std::unique_ptr<aver::editor::SkinDrawTest> skinDraw_;
#if AVER_MODULE_SCENE
    // The scene join: gives every entity with a CSkeletalMesh its own posed mesh. Null when the skinning shader would not compile.
    std::unique_ptr<aver::render::SkinnedScene> skinnedScene_;
#if AVER_MODULE_RENDER_SOFTBODY
    // Beside skinnedScene_: a per-entity vertex source that the scene pass substitutes for the authored mesh.
    std::unique_ptr<aver::render::SoftBodyScene> softBodyScene_;
#if AVER_MODULE_RENDER_HAIR
    std::unique_ptr<aver::render::HairScene> hairScene_;
#endif
#endif
#endif
    std::string skinSceneDir_;
    std::string shaderSourceDir_;                 // --shader-source <dir>, empty = off
    aver::DirectoryWatcher shaderWatch_;    // --skin-scene-test <dir>: where the cooked rig lives
    std::unique_ptr<aver::editor::SkinSceneTest> skinScene_;
    // Direct CPU wall-clock timing around particles::particleSystem().tick(). Accumulated every frame, printed once at shutdown.
    f64  particleTickAccumSec_ = 0.0;
    u64  particleTickFrames_ = 0;
    std::unique_ptr<aver::pt::PtFurnaceTest> ptFurnace_;
    // --pt-scene: the path tracer pointed at the real scene. ptSceneViewWantEnabled_ is the flag that matters.
    std::unique_ptr<aver::pt::PtSceneView> ptSceneView_;
    std::unique_ptr<aver::editor::ReflTest> refl_;
    int  reflBeaconIndex_ = -1;   // which objects_ entry the schedule shows and hides
    rhi::MeshHandle unitCubeMesh_ = 0;   // the editor's own unit cube, half-extent 1
    std::string matSaveStatus_;   // what the last 'Save to C#' did
    // A row asked to be deleted; answered after the tree walk (drawOutlinerRow). GUARDED: type is scene::Entity.
    bool inputProbe_ = false;
    std::string openAsset_;
    std::string selectEntity_;   // --select
    std::string lastOpenAssetPath_;  // last path --open-asset opened; openAsset_ itself is cleared once consumed.
    std::string graphSelectNode_;
    std::string graphTab_; // --graph-select <nodeId>; see setGraphSelectNode's own comment

    editor::ProjectUpgrade pendingUpgrade_;
    // Every PCGVOLUME the loaded level carried, kept verbatim so a save cannot drop them.
    std::vector<fmt::OcPcgVolume> levelPcgVolumes_;
    // The loaded level's own header, placements and PCG volumes stripped. Starting from what the file said, overwriting only what the editor owns.
    // ---- the 3D-viewport icon renderer, and the Player Start marker it draws ----
    // viewportIconsReady_ is the ONE flag the render walk consults to skip the Player Start's cube (false unless the feature AND its texture both came up).
    editor::ViewportIconRenderer viewportIcons_;
    fmt::OcWorldData levelHeader_;
#if AVER_MODULE_PBR
    // pinMaterialResident()/unpinMaterialResident()'s storage -- checked by bindMaterialsForLevel() so a level-scoped release never destroys a material an open tab holds.
    std::unordered_set<std::string> pinnedMaterialNames_;
#endif
    // Whether each level entity's placement said `nocollide`. No component for this: load-time instruction, so without it the save forced `collide = true` on everything.
    std::unordered_map<u32, bool> entityCollide_;
    // Each entity's authored object animation; absent = none. The CAnimator on the entity is only the live clock, so saveLevel reads this map.
    std::unordered_map<u32, EntityAnim> entityAnim_;
    // The Details panel's Animation drag in flight: every selected entity's EntityAnim from the frame the widget was grabbed.
    std::vector<EditCmd::AnimChange> animEditBefore_;
    // Entities placed with `snap`, and the AUTHORED z offset each was placed at. Absent = not snapped.
    std::unordered_map<u32, f64> entitySnapZ_;
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS
    // driveAnimatedBodies' list, kept between frames and rebuilt only when it can have changed.
    std::vector<world::AnimatedBody> animatedBodies_;
    bool animatedBodiesBuilt_ = false;
    usize animatedBodiesFromAnim_ = 0;
    usize animatedBodiesFromBodies_ = 0;
    u64 animatedBodiesFromRev_ = 0;
#endif

    // True while the open level loaded via legacy OCMAP path, not OCWORLD. Reset by unloadLevel.
    bool levelIsLegacyOcmap_ = false;
    // Legacy header for OCMAP-only record kinds (ROOT, CLIENT, SURFACE, GROUND, KILLZ).
    fmt::OcMapData legacyMapHeader_;
    // Entity legacy data: deform flag, surface index, material name. See onLegacyOcmapInstantiated.
    std::unordered_map<u32, bool> entityLegacyDeform_;
    std::unordered_map<u32, i32> entityLegacySurface_;
    std::unordered_map<u32, std::string> entityLegacyMaterial_;
    // Game UI font. Invalid font (missing file) draws nothing.
    ui::UiFont uiFont_;
    rhi::TextureHandle uiFontTexture_ = 0;

    rhi::MeshHandle cubeMesh_=0; u32 cubeTris_=0; int spawnCount_=0;
    // Content index, asset resolvers, mesh registry, materials (shared with runtime).
    game::GameContent content_;
    // Loaded level: parsing, placements, environment, PCG (runtime GameLevel). Entities list is editor-local.
    game::GameLevel level_;
#if AVER_MODULE_SCENE
    // [wave3/level-instances begin] LEVEL INSTANCES (docs/LEVEL_INSTANCES.md): the model is the editor's operations on
    // level_.instances(); the hooks keep levelEntities_, labels and bodies in step; entityPlaceId_ keeps each
    // placement's stable id across saves (overrides are keyed by it).
    editor::LevelInstanceEditorModel levelInstanceModel_{scene::World::instance(), level_.instances(), level_.assets()};
    editor::LevelInstanceCreateDialog levelInstanceDlg_;
    std::unordered_map<u32, u64> entityPlaceId_;
    std::unordered_map<u32, std::string> entityAuthoredName_;   // an instance member's authored `name`, for a commit
    bool capturingInstance_ = false;                             // fillPlacementFromEntity may read members (a commit)
    scene::Entity instanceSelPinned_ = scene::kInvalidEntity;    // the selected instance stays loaded while it is selected
    void installLevelInstanceHooks();
    editor::LevelInstanceUiCallbacks levelInstanceUiCallbacks();
    void pushLevelInstanceEdit(const editor::LevelInstanceEdit& edit);
    bool captureEntityForInstance(scene::Entity e, fmt::OcWorldData& into, i32 parentIdx);
    void openInstanceLevel(const std::string& assetRef);
    void levelInstanceBack();
    bool bakeInstancePack(scene::Entity root, std::string* why);
    void keepPlacementIds(fmt::OcWorldData& w, const std::unordered_map<u32, i32>& slotOf);
    void writeLevelExtent(fmt::OcWorldData& w);
#  ifdef AVER_WITH_LEVELINST_ABI
    AverLevelInstanceHost levelInstHost_{};   // the script seam onto level_.instances()
#  endif
    // [wave3/level-instances end]
#endif

#if AVER_MODULE_LANDSCAPE
    // Terrain section: render, collision, authoring (shared with runtime).
    game::GameLandscape landscape_;

#endif

#if AVER_MODULE_SCENE
    // Chunk streaming on/off (opt-in via Window > Chunk Streaming).
    static f32 fogDensityForOpacityAt(f32 distanceCm, f32 targetOpacity);

#if AVER_MODULE_SCENE
    bool anyChunkWorldOwns(scene::Entity e) const;
#endif

    void setChunkStreamingEnabled(bool on);

    void setDroneEnabled(bool on);

    u64 residentTriangleCount() const;

    // Streaming status readout (pendingLoads/failedLoads show working/broken state).
#if AVER_WITH_IMGUI
    void buildChunkStreamingPanel();
#endif  // AVER_WITH_IMGUI

    static bool onOpenRequestThunk(void* user, const char* path);

    bool handleOpenRequest(const char* path);

    // Load level via GameLevel (parses file, instantiates placements).
    void loadLevel(Engine& eng, const std::string& path);

    // Hooks after loadLevel: editor setup (labels, body map, camera, GI). One per file kind.
    void onLevelInstantiated(const game::GameLevel::LoadedLevel& loaded);
    void onLegacyOcmapInstantiated(const game::GameLevel::LoadedLevel& loaded);

#if AVER_MODULE_SCENE
    // LEVEL STREAMING (SandboxLevelStream.cpp, docs/LEVEL_STREAMING.md).
    void installLevelStreamHooks(Engine& eng);
    void onStreamedIn(const std::vector<scene::Entity>& es, const std::vector<u32>& placements,
                      const std::vector<i32>& bodies);
    void onStreamedOut(scene::Entity e, u32 placement, bool writeBack);
    bool streamRecordFromEntity(scene::Entity e, const std::unordered_map<u32, const Transform*>* animPlaced,
                                fmt::OcWorldPlacement& p, bool& keepParent);
    void adoptNewLevelEntities();
    void appendStreamedSequenceTracks(fmt::OcWorldData& w);
    // [wave5/sequencer] The name a sequence track binds an actor by: its outliner label when no other actor shares it.
    std::string sequenceBindName(scene::Entity ent) const;
    bool regenerateStreamData(std::string& report);
    void refreshStreamRecord(scene::Entity e, u32 placement, fmt::OcWorldPlacement p);
    void pinStreamed(scene::Entity e);
    void onStreamedEntityDestroyed(scene::Entity e);
    void tickLevelStreaming(f32 dt);
    void buildStreamedPlacements(fmt::OcWorldData& w, const std::unordered_map<u32, const Transform*>& animPlaced,
                                 std::unordered_map<u32, i32>& slotOf);
    bool fillPlacementFromEntity(scene::Entity e, fmt::OcWorldPlacement& p,
                                 const std::unordered_map<u32, const Transform*>* animPlaced);
    bool entityWorldBounds(scene::Entity e, f32 outMin[3], f32 outMax[3]);
    void stampStreamFields(fmt::OcWorldData& w, const std::unordered_map<u32, i32>& slotOf);
    void buildLevelStreamingSettings();
    // LARGE_WORLD phase 2 (SandboxOrigin.cpp): the floating origin. tickOrigin is slot S0, the first thing onUpdate does.
    game::OriginParticipants origin_;
    void initOrigin(Engine& e);
    void configureOrigin();
    void tickOrigin(Engine& e);
    // The editor's part (SandboxEditorOrigin.cpp): the gesture gate and the rebasers of order 80, the world.* console host, the
    // overlays a shift stales, and the Show > World Origin window.
    editor::EditorOriginParticipant editorOrigin_;
    editor::WorldConsoleHost worldHost_;
    void initEditorOrigin();
    void afterOriginShift(Engine& e);
    bool originBlocksSave(const char* what);
    std::string streamRegenReport_;
    // LARGE_WORLD 2.4.5 (SandboxLevelCook.cpp): World Settings > Level Streaming > Cook Chunks, and the freshness check before a Standalone launch.
    bool cookLevelChunks(std::string& report);
    bool refreshCookedChunksForPlay();
    void buildCookChunksSettings();
    // World Settings > Level Streaming > HLOD (SandboxLevelHlod.cpp): a background bake of the per-cell proxies.
    void buildHlodSettings();
    editor::ModelingMode modeling_;   // SandboxModeling.cpp / ModelingMode.cpp
    void handleModelingInput(const ImGuiIO& io, bool overScene, f32 mx, f32 my);
    void installPcgEditorHost();   // SandboxPcg.cpp: the PCG Graph tab's landscape / Generate / Clean up
    void cbCreatePcgGraph();       // SandboxPcg.cpp: a starter PCG graph
    void buildPhysAnimSection();   // SandboxPhysAnim.cpp: Simulate / Animate mode panels, Apply Hit
    void startHlodBuild();
    void pollHlodBuild();
    editor::HlodBuildJob hlodJob_;
    std::string hlodReport_;
    bool hlodForce_ = false;
    std::string cookChunksReport_;
    usize streamSeenCount_ = 0;                  // levelEntities_ size at the last adoption pass
    std::unordered_set<u32> streamAdopted_;      // checked by adoptNewLevelEntities (adopted or not a placement)
    std::unordered_set<u32> streamSelPinned_;    // pinned because selected
    bool streamPlayWas_ = false;
    std::unordered_set<u32> streamPlayResident_; // streamed entities resident when Play started
    std::vector<i32> streamOutSlot_;             // record index -> saved placement line, last save
    std::vector<fmt::OcSeqTrack> streamSeqTracks_;   // object tracks of a streamed level, kept aside
    std::vector<std::string> streamSeqOwner_;        // [wave5/sequencer] the sequence (by name) each aside track belongs to
    std::vector<fmt::OcSequence> streamSeqHeaders_;  // the level's sequences without tracks, to rebuild them on save
    // Label each streamed entity got when it streamed in (a changed label is a rename), and what is pinned.
    std::unordered_map<u32, std::string> streamLabel_;
    std::unordered_set<u32> streamPinned_;
#  if AVER_MODULE_VOXI
    game::LevelFoliage levelFoliage_;
    // The start level's foliage, kept until the renderer attaches (see afterInstantiate).
    fmt::OcWorldData foliagePendingWorld_;
    std::vector<std::string> foliagePendingTables_;
    bool foliagePending_ = false;
#  endif
#endif

    // SandboxUpdate.cpp: the steps onUpdate runs, in order
    void applyConsoleSets(Engine& e, const Timestep& t);
    void tickViewModeAndCameraDrift(const Timestep& t, bool& debugViewActiveThisFrame);
    void tickCameraAutomation(Engine& e, const Timestep& t, HitchMarks& hm);
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
    void reassertOcclusionConsole();
#endif
    void pollAutosave(Engine& e, const Timestep& t);
    void tickLaunchRequests(const Timestep& t);
    void tickAutoFrameCommands(Engine& e, const Timestep& t);
    void tickPathTracingAutomation();
#if AVER_MODULE_MCP
    void pumpMcp();
#endif
    void tickAmbientState(const Timestep& t);
#if AVER_WITH_IMGUI
    void tickDpiAndPlayWindow(Engine& e);
#endif
#if AVER_WITH_IMGUI
    void resolveEditorInputOwnership(Engine& e);
#endif
#if AVER_WITH_IMGUI
    void tickEditorFlyCamera(const Timestep& t);
#endif
#if AVER_MODULE_FRAMEWORK
    void tickPlayMouseCapture(Engine& e);
#endif
#if AVER_MODULE_FRAMEWORK
#if AVER_MODULE_SCRIPTING
    void reportHudPreview();
#endif
#endif
#if AVER_MODULE_FRAMEWORK
    void publishWindowState(Engine& e);
#endif
#if AVER_MODULE_FRAMEWORK
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
    void submitSubViewports();
#endif
#endif
#if AVER_MODULE_FRAMEWORK
    void feedUiPointer();
#endif
#if AVER_MODULE_FRAMEWORK
    void tickGameUi(Engine& e, const Timestep& t);
#endif
#if AVER_MODULE_FRAMEWORK
    void tickGameplay(Engine& e, const Timestep& t, const f32 gameDt);
#endif
#if AVER_MODULE_FLUIDS
    void tickWater(Engine& e, HitchMarks& hm, const f32 gameDt);
#endif
#if AVER_MODULE_SCENE
    void tickObjectAnimation(Engine& e, HitchMarks& hm, const f32 gameDt);
#endif
#if AVER_MODULE_SCENE
#if AVER_MODULE_SOFTBODY_SCENE
    void tickSoftBodySim(HitchMarks& hm, const f32 gameDt);
#endif
#endif
#if AVER_MODULE_SCENE
#if AVER_MODULE_PHYSICS
    void tickDriveBodies(HitchMarks& hm, const f32 gameDt);
#endif
#endif
#if AVER_MODULE_SCENE
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    void tickParticles(const f32 gameDt);
#endif
#endif
#if AVER_MODULE_SCENE
    void updateRenderScenes(Engine& e, HitchMarks& hm);
#endif
#if AVER_MODULE_SCENE
    void tickThumbnailsAndTestHooks(Engine& e);
#endif
#if AVER_MODULE_SCENE
#if AVER_MODULE_SCRIPTING
    void tickGraphs(const Timestep& t, HitchMarks& hm, const f32 gameDt);
#endif
#endif
#if AVER_MODULE_SCENE
    void tickChunkAndLevelStreaming(const Timestep& t, HitchMarks& hm);
#endif
    void resolveViewOverride(const Timestep& t);
    void applyDeviceRenderSettings(Engine& e, const Timestep& t);
    void setupCameraAndSky(Engine& e);
    void pushPostSettings(Engine& e, bool debugViewActiveThisFrame);

    // SandboxInit.cpp: the steps onInit runs, in order
    void applyStartupRenderSettings(Engine& e);
    void addCoreRenderFeatures(Engine& e);
    void registerAssetEditors();
    void installAssetEditorHooks();
    void installActorEditorHooks();
    void bindWindow(Engine& e);
#if AVER_MODULE_MCP
    void startMcpChannel();
#endif
    void startRuntimeSystems();
    void installGameProviders();
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    void initLodMeshShader(Engine& e);
#endif
    void openStartupProject(Engine& e);
#if AVER_WITH_IMGUI
    void initEditorUi(Engine& e);
#endif
    void createDefaultScene(Engine& e);
    void createGizmoMeshes(Engine& e);
#if AVER_MODULE_SCENE
    void initSceneRenderers(Engine& e);
#endif
#if AVER_MODULE_FLUIDS
    void initWater(Engine& e);
#endif
#if AVER_MODULE_LANDSCAPE
    void installLandscapeProviders();
#endif
#if AVER_FLUIDS_SIMULATED
    void initViewportIcons(Engine& e);
#endif
    void setupFurnaceTest();
    void setupReflectionTest();
    void setupSkinDrawTest(Engine& e);
#if AVER_MODULE_SCRIPTING
    void initScriptingHost();
#endif
#if AVER_MODULE_SCRIPTING
#if AVER_MODULE_VOXI
    void initVoxiBackend(Engine& e);
#endif
#endif
#if AVER_MODULE_SCRIPTING
    void initGameUi(Engine& e);
#endif
#if AVER_MODULE_SCRIPTING
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
    void initOcclusionCuller(Engine& e);
#endif
#endif
#if AVER_MODULE_SCRIPTING
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    void initParticleRenderer(Engine& e);
#endif
#endif
#if AVER_MODULE_SCRIPTING
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    void initWaterRenderer(Engine& e);
#endif
#endif
#if AVER_MODULE_SCRIPTING
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    void setupParticleScenes();
#endif
#endif
#if AVER_MODULE_SCRIPTING
    void setupGpuSelfTests(Engine& e);
#endif
    void setInitialToolAndCamera();

    // SandboxRender.cpp: the steps onRender runs, in order
    void updateCpuTimingEnable();
    void tickProjectLoadingScreen();
#if AVER_MODULE_VOXI
    void tickLevelPrepToast(Engine& e);
#endif
    void drawEditorObjects(Engine& e);
#if AVER_MODULE_FLUIDS
    void drawWater(Engine& e);
#endif
#if AVER_MODULE_LANDSCAPE
    void drawLandscape(Engine& e);
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_VOXI
    void updateSceneFeeds();
#endif
#if AVER_MODULE_SCENE
    void beginPlayMobilityFrame();
#endif
#if AVER_MODULE_SCENE
#if AVER_MODULE_TRIFACTOR
    void resetLodFrameState(Engine& e);
#endif
#endif
#if AVER_MODULE_SCENE
#if AVER_MODULE_VOXI
    void drawDepthPrepass(Engine& e);
#endif
#endif
#if AVER_MODULE_SCENE
    void logSceneRenderStats(const aver::game::SceneDrawStats& colourStats);
#endif
#if AVER_MODULE_SCENE
    void drawSceneEntities(Engine& e);
#endif
    bool drawSelectionOutline(Engine& e);
#if AVER_MODULE_SCENE
    void drawPlayerStartMarker(Engine& e, const bool anyPlaying);
#endif
#if AVER_MODULE_SYNAPSE
    void drawNavMesh(Engine& e);
#endif
#if AVER_MODULE_SYNAPSE_SCENE
    void drawAiDebug(Engine& e);
#endif
#if AVER_MODULE_SCENE
    void drawMotionModelingCameraDebug(Engine& e);
#endif
#if AVER_MODULE_SOFTBODY_SCENE
    void drawSoftBodyDebug(Engine& e);
#endif
#if AVER_MODULE_VEHICLE_SCENE
    void drawVehicleDebug(Engine& e);
#endif
#if AVER_MODULE_FRAMEWORK
    void drawClassMarkers(Engine& e);
#endif
#if AVER_MODULE_PHYSICS
    void drawPhysicsDebug(Engine& e);
#endif
    void drawEditorGizmos(Engine& e);
    void drawDebugLabels();
    void tickReflectionTest(Engine& e);

#if AVER_MODULE_FRAMEWORK
    void spawnClassPlacements();
    void reportSlowGameplay(f64 ms);   // SandboxUpdate.cpp
    // Place Class (docs/plans/parity_demo/PLACE_CLASS.md): bodies are in SandboxLevelLoad.cpp / SandboxViewport.cpp.
    scene::Entity spawnClassSlot(usize slot, const Transform* worldXf);
    scene::Entity placeClassInstance(fmt::OcWorldPlacement p, std::string* why);
    void dropGraphClass(const std::string& fullPath, f32 screenX, f32 screenY);
    void drawGraphActorMenu();                 // body of the Add submenu
    int  classSlotOfEntity(scene::Entity e) const;      // index into levelClassInstances_, -1 if not a live root
    scene::Entity classRootOf(scene::Entity hit) const; // hit's class root or kInvalidEntity
    void tagClassEdit(EditCmd& c, scene::Entity e);
    void respawnClassFromEdit(const EditCmd& c);
    void respawnLostClassInstances();
    bool classPartsBounds(scene::Entity root, Vec3& lo, Vec3& hi) const;  // world box of the root's mesh parts
#endif

    void applyLevelSky(const fmt::OcWorldData& w);

#if AVER_MODULE_VOXI
    // Fits the GI volume to the loaded level's bounds (level_.placementBounds).
    void fitGiVolumeToLevel();
#endif

    // Frames the editor camera on the loaded level's bounds (level_.placementBounds).
    void frameCameraOnLevel();

    // Camera view persistence per level (LevelViewStore.hpp, LevelViews.ini).
    void storeLevelView();
    bool lookupLevelView(const std::string& levelPath, editor::LevelView& out) const;
    // Apply view with mouse-look and wheel clamps.
    void applyLevelView(const editor::LevelView& v);
    std::string levelViewsPath() const;

    bool requestOpenLevel(const std::string& path, const char* why);

    void applyPendingOpen(Engine& eng);

    void startNewLevel(Engine& eng);

    void openLevelDirect(Engine& eng, const std::string& path);

    void loadStartMap(Engine& eng);

    void unloadLevel(Engine& eng);

    // `playSnapshot`: Play's own copy of the level -- no "saved" mark, no prefab asset writes, no autosave retire.
    bool saveLevel(const std::string& path, bool playSnapshot = false);

    bool saveLevelAsOcmap(const std::string& path, bool playSnapshot = false);

    // EXITING PLAY RELOADS THE LEVEL (owner decision 2026-10-12): startPlay writes the level as it stands,
    // unsaved edits included, to pieSnapshotPath_; stopPlay destroys every entity born during Play and the
    // next frame reloads that copy under levelPath_ (GameLevel::setNextLoadSource), so sidecars resolve as
    // usual. Undo history does not survive it. The transform restore below still runs first, as a fallback.
    void writePlaySnapshot();
    void destroyPlaySpawned();
    void reloadAfterPlay(Engine& eng);
    // LEVEL TRAVEL IN PLAY (Godot's change_scene_to_file, Unreal's OpenLevel in PIE): the requested level replaces the
    // open one for the rest of the session, and Stop reloads the Play-start copy under the level Play began on
    // (pieEditedLevelPath_). pieTravelling_ is set while the swap runs, so unloadLevel neither stops the session nor
    // stores a view for the level being left.
#if AVER_MODULE_FRAMEWORK
    void travelDuringPlay(Engine& eng, const std::string& requested);
    void raiseFlyingPawnToEye();
#endif

    using PlaySavedTransform = PlayState::PlaySavedTransform;
#if AVER_MODULE_SCENE
    // The level's sequence (Animate mode, Play). Its camera and emissive reach the frame through
    // viewOverride_ and DrawWorldOptions::emissiveScale.
    editor::SequenceEditor seqEditor_;
    std::string seqHooksDir_;   // [wave5/sequencer] the content dir seqEditor_'s audio / event hooks were built for
    struct SequenceRunState {
        enum class Phase : u8 { Off, Waiting, Running, Draining, Done };
        SequenceRunArgs args;
        Phase phase = Phase::Off;
        u32 waited = 0, readyFrames = 0;
        i64 frame = 0, frames = 0;      // the frame being shown, and frames in the pass
        bool frameSet = false;
        i64 pendingFrame = -1;          // a capture requested for this frame and not yet read
        u64 pendingEngineFrame = 0;
        u32 tries = 0, written = 0, missed = 0;
        bool failed = false;
        // ---- a Movie Render Queue job (renderJobStep); isJob false = the plain capture above ----
        bool isJob = false;
        bool owns = false;               // the screen is the render: buildUI draws nothing
        bool exitWhenDone = true;        // the command line ends the process; the panel returns to the editor
        bool planned = false, levelRequested = false, cancelRequested = false;
        bool rewind = false;             // a capture was lost: pose the same sample again
        bool warnedPt = false;
        fmt::RenderJob job;
        std::string jobDir;              // relative output paths start here
        std::string loadError;
        std::string status;              // the last job's one-line result, for the panel
        bool statusIsError = false;
        i32 sub = 0;                     // temporal sample of `frame` being shown
        u32 hold = 0;                    // engine frames the current pose has been on screen
        bool sampleRequested = false;    // the current sample's capture is in flight or read
        i32 pendingSub = 0;
        u32 capW = 0, capH = 0;          // the size the pending capture must come back as
        f32 capX = 0, capY = 0;
        u32 retries = 0;
        i64 samplesDone = 0, samplesTotal = 0;
        EditorMode prevMode = EditorMode::Select;
        bool prevChrome = false;
        std::vector<u8> savedWindow;     // the window placement to restore (Windows)
    } seqRun_;
    editor::RenderRun renderRun_;
    editor::RenderQueuePanelState renderQueue_;
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS
    // Vehicles as physics cars while Play runs. Built in capturePlayWorld, ended in stopPlay.
    world::VehicleSystem vehicles_;
#endif

    std::vector<scene::Entity> levelEntities_;

#if AVER_MODULE_FRAMEWORK
    // Class placements: level_.classPlacements() has the list; spawnClassPlacements fills this.
    // Each instance carries its placement id explicitly, not by position.
    using ClassInstance = editor::LevelClassInstance;
    std::vector<ClassInstance> levelClassInstances_;
    std::vector<fmt::OcWorldPlacement> classAuthored_;      // every class placement of the open level (loaded first, session-added appended)
    std::vector<i64> classAuthoredIndex_;                   // parallel: its placement index in the loaded file, -1 if added this session
    game::PlacementTree placementTree_;                     // the loaded file's hierarchy: placed actors spawn under their parents
    using PlayClassSaved = PlayState::PlayClassSaved;
    std::vector<editor::GraphClassDecl> classPickList_;
    bool classPickDirty_ = true;                           // rescan when the Add popup opens
#endif

    // PCG chunk streaming (opt-in, transient entities).
    game::GameStreaming streaming_;
#if AVER_MODULE_PHYSICS
    std::vector<int32_t> levelBodies_;
#endif

    // Graph-driven drone (opt-in, transient). Spawned by setDroneEnabled, ticked via scripts_.graphTick.
    scene::Entity droneEntity_ = scene::kInvalidEntity;
    // True if Play started this drone (vs user enabling from Window > Drone).
    bool droneStartedByPlay_ = false;
    // .ocgraph path (relative to Content; set only by --drone-graph).
    std::string droneGraphRel_;
    bool droneGraphLoaded_ = false;
    f32  droneTimeSeconds_ = 0.0f;
    Vec3 dronePos_{};
    Vec3 droneVel_{};
    Vec3 droneLastPos_{};
    bool droneHaveLastPos_ = false;         // False right after enabling; first velocity sample is zero.
    u32  droneLogsLeft_ = 30;               // More than streaming's 8.
#endif // AVER_MODULE_SCENE

    // Mouse capture (hidden, confined, re-centred each frame while playing).
    game::MouseCapture mouse_;
    Window* window_ = nullptr;   // borrowed from the engine in onInit, for the HWND

    // Editor's input accumulator (tested aver::InputState, separate from ImGui).
    InputState input_;

    // Who owns keyboard and mouse this frame (recomputed before pushInput).
    editor::InputOwnership own_;

    // Prevent viewport click from counting as both UI action and trigger pull.
    bool eatRecaptureClick_ = false;

#if AVER_WITH_IMGUI
    editor::UiRegistry uiReg_;   // what the editor drew this frame, by name
#endif
#if AVER_MODULE_MCP
    mcp::McpBridge mcp_;         // Inert until --mcp.

    void applyMcpCommand(const mcp::Command& c);
    // The pointer an MCP move or click put down, held in ImGui for a moment: with the editor behind another window, the posted move's
    // mouse-leave tracking fires at once and ImGui would forget the position before the click lands.
    void holdMcpPointer();

    void registerMcpAbis();

    // Level ABI: spawn/transform/material/visibility/etc. via editor paths (undoable, dirties level).
    bool mcpLevelAbi(const mcp::AbiCall& a, std::string& result, std::string& why);
    bool mcpLevelPlace(const std::string& text, std::string& result, std::string& why);
    bool mcpLevelSave(const std::string& target, std::string& result, std::string& why);
    bool mcpLevelMove(AvId e, const EditXform& x, std::string& why);
    bool mcpLevelEntity(f64 id, AvId& out, std::string& why) const;

    // Start MCP channel (register ABIs, install hooks, listen). ABIs registered once only.
    bool mcpStart(u16 port);
    void mcpStop();
#endif // AVER_MODULE_MCP
    bool releasedByUser_ = false;   // Shift+F1 during a session; cleared when the session ends

    using PlayMode = PlayState::PlayMode;
    using PlaySpawnAt = PlayState::PlaySpawnAt;
    // Start Play (refused while active, like button). Standalone goes through launchInRuntime.
    void launchPlay(Engine& e, PlayMode m);
    // Play From Here: viewport pawn starts at surface, facing camera yaw.
    void playFromHere(const Vec3& surface);
    // Play in New Window: the device mirrors the game view into playWindow_ (IDevice::setMirrorWindow),
    // whose input feeds input_. The scene renders at the window's size; see docs/editor/PLAY_IN_NEW_WINDOW.md.
    std::unique_ptr<Window> playWindow_;
    void openPlayWindow(Engine& e);
    void closePlayWindow();
    void updatePlayWindow(Engine& e);   // per frame, before the scene rect is used
    bool playWindowFocused() const { return playWindow_ && playWindow_->isForeground(); }
    Window* captureWindow() const { return playWindow_ ? playWindow_.get() : window_; }
    // Toggle eject (F8). No-op outside framework session.
    void togglePlayEject();
    // Teleport pawn to camera (Shift+F while ejected).
    void teleportPawnToCamera();
    // Frame Skip (only while paused).
    void requestPlayFrameStep();
    bool playEjected() const;
    bool hasPossessedPawn() const;  // True while controller 0 possesses a live pawn.

    void setMouseCaptured(bool on);
    void pollCapturedMouse();

    bool playSessionActive() const;
    std::mutex          logMutex_;
    using GraphPrint = DrawerState::GraphPrint;
    void cbColsEnsure();
    void cbColsSave();
    std::string cbKeyOf(const std::string& full) const;
    void drawCbChips();
    void drawCbSmartTree();
    void cbCollectionMenu(const std::string& full);
    // Revision control: git queries run on detached worker thread, draw latches results.
    struct RcStatusQuery {
        std::atomic<bool> done{false};
        std::string dir;                  // Project directory asked about; checked on reap.
        std::string root;                 // Git toplevel path (empty = not a repo).
        editor::RepoStatus status;
        std::string why;                  // Git error (empty = "not a repository").
        bool gitPresent = false;
    };
    // One path's history and diff. Fetched together so both show for the clicked row.
    struct RcFileQuery {
        std::atomic<bool> done{false};
        std::string root;                 // Repository asked; checked on reap.
        std::string path;                 // Repo-relative, git spelling.
        std::vector<editor::LogEntry> log;
        std::string logWhy;
        // Diff split on worker thread, not draw frame.
        std::vector<std::string> diff;
        std::string diffWhy;
        bool wantDiff = false;            // False for files viewer skips.
    };
    std::shared_ptr<RcStatusQuery> rcStatusJob_;
    std::shared_ptr<RcFileQuery>   rcFileJob_;
    // Window > Data Layers (SandboxDataLayers.cpp). Members are keyed by EditId so an undone delete finds its layers again.
    editor::dl::Model   dataLayers_;
    editor::DataLayersUi dataLayersUi_;
    std::unordered_set<u32> dataLayerHidden_;   // entities the layer toggles hid (a subset of editorHidden_)
    void drawDataLayersPanel();
    void pushDataLayerEdit(editor::dl::Edit e);
    void applyDataLayerEdit(const editor::dl::Edit& e, bool undoing);
    void syncDataLayerVisibility();
    void loadDataLayers(const fmt::OcWorldData& w);
    void clearDataLayers();
#if AVER_MODULE_SCENE
    // Mesh id -> project-relative path. Handles/bounds/parts are in content_.
    std::unordered_map<u64, std::string>     meshPathById_;
    // Entities missing mesh or with invisible bit. Per-entity or per-id fault. Never cleared on reload.
    // Invisible: last generation reported per entity slot (0 = none); generations only rise per slot.
    std::vector<u8> undrawnInvisibleGen_;
    std::unordered_set<u64> undrawnMissingMesh_;
    // Triangle count per mesh id (same key as content_). Enables budget reporting instead of guessing.
    std::unordered_map<u64, u32> meshTris_;
    // Skeletal mesh ids (have skin weights, colored differently in Content Browser).
    std::unordered_set<u64> skinnedMeshIds_;
    // Pick geometry per mesh (lazy-loaded on first click). Empty = tried and unavailable.
    std::unordered_map<u64, aver::editor::PickGeometry> pickGeometry_;
    int lastSceneDrawn_=-1;           // Last scene-entity draw count (startupComplete settle input).
    // Settle detector for startupComplete.
    mutable int startupSettleCount_ = -2;   // -2 so cannot match lastSceneDrawn_'s -1.
    mutable int startupSettleFrames_ = 0;
#endif  // AVER_MODULE_SCENE
    // Project-open loading screen (alive from applyProject until scene settles).
    std::unique_ptr<struct LoadingScreen> projectLoading_;
    // Frames the project loading screen has been up (capped in onRender).
    int projectLoadingFrames_ = 0;
#if AVER_MODULE_SCENE
    int lastSceneCulled_=-1;          // Cull count.
    int lastSceneOwnerHidden_=-1;     // Owner-hide count.
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    using MeshLodLadder = LodState::MeshLodLadder;
#endif

#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    using LodSelectStats = LodState::LodSelectStats;

    using MeshClusterData = LodState::MeshClusterData;

    using ClusterCutCache = LodState::ClusterCutCache;

    using LodClusterStats = LodState::LodClusterStats;

    // GPU per-cluster LOD (--lod-mesh-shader). ASMain/MSClusterMain runs cut test per cluster.
    // Exists alongside CPU path (never replacing it). Mesh-shader-tier-0 devices use CPU path.

    using MeshClusterGpu = LodState::MeshClusterGpu;

    using ClusterFrameCB = LodState::ClusterFrameCB;

    using LodMeshShaderStats = LodState::LodMeshShaderStats;
#endif

    // Depth proxy map (populated if Trifactor, empty if not). Each lookup answers 0 if no entry.
    std::unordered_map<rhi::MeshHandle, rhi::MeshHandle> depthProxy_;

    static rhi::MeshHandle depthProxyLookup(rhi::MeshHandle mesh, void* user);

    u32 sceneWalkReports_ = 0;     // Scene walk count (cost split reports at 2^n).

};

} // namespace aver
