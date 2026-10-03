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
#include "aver/rhi/ShaderCacheSweep.hpp"
#include "aver/core/Log.hpp"
// The runtime's content component (Runtime/), which the editor uses rather than keeping its own copy.
#include "aver/game/GameContent.hpp"
#include "aver/game/GameLevel.hpp"
#include "aver/game/GameWater.hpp"
#include "aver/game/GameStreaming.hpp"
#include "aver/game/GameLandscape.hpp"
#include "aver/game/MouseCapture.hpp"
#include "aver/game/PlayMobility.hpp"
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
// Historical name: set whenever Aver.Audio.Abi links in ("mixer seam present"), not "sound editor
// wants audio" (named when SoundEditor's preview button was the only thing opening a device). Not
// renamed to AVER_SANDBOX_AUDIO: tests/editor relies on it being undefined.
#  include "aver/audio/audio_abi.h"
#endif
#include "aver/formats/OcProject.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/formats/OcMesh.hpp"
// Behaviour-tree format for the Content Browser's "New Behaviour Tree". .ocbt is a binary AVR1
// container (not text), so a starter must go through fmt::saveOcBt, unlike a starter .ocgraph.
#include "aver/formats/OcBt.hpp"
#include "aver/formats/GltfImport.hpp"
#if AVER_MODULE_TRIFACTOR
#include "aver/trifactor/ClusterAdapt.hpp"
#endif
// Quadtree-LOD heightfield renderer, independent of AVER_MODULE_SCENE (a section is not an ECS
// entity). See the landscape_ member block below for how it's hosted.
#if AVER_MODULE_LANDSCAPE
#include "aver/formats/OcLand.hpp"
#include "aver/formats/OcFoliage.hpp"
#include "aver/landscape/LandscapeTree.hpp"
#include "aver/landscape/LandscapeRenderer.hpp"
// The sculpt editor: a cursor ray against the section's surface, and the brush edits themselves.
// Both are RHI-free, same as the rest of Aver.Landscape -- see their own headers.
#include "aver/landscape/HeightfieldRay.hpp"
#include "aver/landscape/Sculpt.hpp"
// Terrain collision. Aver.Landscape does the conversion; the physics ABI takes the result. Both
// halves already existed and tests/landscape proves the pair with a real Jolt raycast -- nothing
// called it.
#include "aver/landscape/PhysicsBridge.hpp"
// The infinite fill past an authored section's own rim: terrainHeightAt (TerrainNoise.hpp) and the
// tile coordinate + procedural-section synthesis (TerrainTile.hpp) that let game::GameLandscape keep
// a small ring of streamed sections resident around the camera. See its updateRingTiles().
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
// Unguarded here (not in the scene block): ThumbnailCache depends only on RHI/ActorPreview, names
// no scene type, and ThumbnailCache.cpp is unconditional in the source list -- so this line, and with
// it the thumbnails_ declaration, was the only thing a scene-less build lost.
#include "ThumbnailCache.hpp"
#if AVER_MODULE_SCENE
#include "aver/render/SkinnedScene.hpp"
#endif
#include "aver/ui/ui_abi.h"

#include "ProjectBrowser.hpp"
#include "LevelClassSave.hpp"
#include "LevelList.hpp"
#include "ProjectScaffold.hpp"
#include "GraphAssetPresentation.hpp"
#include "MaterialResolve.hpp"
#include "SurfaceName.hpp"
#include "ClusterMaterialShader.hpp"
// F1 (occlusion-fix-plan.md): the one place the "which route delivers this entity's draws" rule
// lives; see its own top comment. Pulled via Aver.Runtime.Game.Core's public include dir so both
// hosts share it, instead of Runtime/src/GameRender.cpp's old hand-copied second statement -- same
// resolution aver/game/GameContent.hpp gets at line 18. A pure header, like PtRenderConflict.hpp.
// WARNING: aver::game::SurfaceLook here is a DIFFERENT type from this file's own unqualified
// SurfaceLook further down (line 24819) -- always write it qualified; class-scope lookup makes a bare
// `SurfaceLook` inside a SandboxApp member resolve to the nested type, never this header's.
#include "aver/game/SceneSubmission.hpp"
// pick()'s ray/triangle math (positions/normals/indices) factored into its own testable header --
// see ViewportPick.hpp's top comment; a pure header for the same reason SceneSubmission.hpp is one:
// otherwise untestable inside a 29,000-line file with no header of its own.
#include "ViewportPick.hpp"
// The profiler panel's Play-side numbers (smoothed frame time, per-phase CPU cost); pure std, so no guard.
#include "PlayProfile.hpp"

// The material sampler register on the cluster pipeline (materialShaderDefines() gets the same
// number). Fixed at s0 so it never moves whether or not AVER_MODULE_VOXI is compiled in -- Voxi's own
// volume/shadow samplers start at s1 instead of reusing s0/s1.
namespace { constexpr aver::u32 kClusterMaterialSamplerSlot = 0; }
#if AVER_MODULE_VOXI
// Stage 3: where cluster pipeline table 0 puts Voxi's merged GI/shadow resources, after the 4
// cluster-geometry SRVs (t0..t3). Full map in ensureLodMeshPipeline; consumers in VoxiGiShaders.hpp.
namespace {
constexpr aver::u32 kClusterGiSrvBase       = 4;   // t4 GI volume, t5 shadow map
constexpr aver::u32 kClusterGiSamplerBase   = 1;   // s1 volume (linear-clamp), s2 shadow (comparison)
// b3, not kFeatureFrameConstantRegister (b4): the AS/MS half already owns b4 for ClusterFrameCB, and
// one root signature has one cbuffer per register regardless of stage. b3 is free (see
// ensureLodMeshPipeline's constantDwords[3]).
constexpr aver::u32 kClusterGiFrameRegister = 3;
}
#endif
#include "ToolsMenu.hpp"
#include "UiRegistry.hpp"
#if AVER_MODULE_SYNAPSE
#include "NavBakeCommand.hpp"
#endif

#if AVER_MODULE_MCP
#include "aver/mcp/McpBridge.hpp"
#endif
#include "ToolGlyphs.hpp"
#include "EditorWidgets.hpp"
#include "FoliageAlign.hpp"
#include "PlayerStartRefresh.hpp"
#include "LevelViewStore.hpp"
#include "AssetEditor.hpp"
#include "ActorEditor.hpp"
#include "AnimEditor.hpp"
#include "GraphEditor.hpp"
#include "BtEditor.hpp"
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
// Unconditional for the identical reason: OcInput.hpp's own comment states it needs no engine
// dependency either.
#include "InputSchemeEditor.hpp"
#include "EditorEuler.hpp"
#include "AssetRefScan.hpp"
#include "EditorTransform.hpp"   // dropRestLift, so a dropped asset rests on what it landed on
#include "EditorNotifications.hpp"
#include "EditorPrefs.hpp"
#include "EditorIcons.hpp"
#include "EditorKeybinds.hpp"
// The one place that decides who owns the keyboard and mouse this frame.
#include "ViewportIconRenderer.hpp"
#include "PhysicsSceneSync.hpp"
#include "InputOwnership.hpp"
#include "PtRenderConflict.hpp"
#include "AverSrChoice.hpp"
#include "EditorConsole.hpp"
#include "EditorEntitySnapshot.hpp"
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

// Fluids and Soft-Body are INDEPENDENT of Particles (see modules/fluids and render.softbody
// CMakeLists.txt) -- each guarded by its own module flag, matching use sites below
// (AVER_MODULE_RENDER_SOFTBODY && AVER_MODULE_SCENE at softBodyScene_, AVER_FLUIDS_SIMULATED in
// GameWater.hpp). b6881c49 nested all four headers under `AVER_MODULE_PARTICLES && AVER_MODULE_SCENE`,
// which broke a particles-off tree (~40 errors) while the members/calls they declare stayed compiled in.
#if AVER_MODULE_RENDER_SOFTBODY && AVER_MODULE_SCENE
#include "aver/render/SoftBodyScene.hpp"
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

// Outside the PBR block on purpose, unlike TextureUpload.hpp beside which it was included:
// LevelSky.hpp maps a level's weather onto an rhi::SkyAtmosphere and names no material -- its own top
// comment says "a build without it still has an rhi::SkyAtmosphere to fill" -- so PBR=OFF still
// needs it: captureLevelEnv saves the sky/fog regardless.
#include "aver/assets/LevelSky.hpp"

#if AVER_MODULE_SCRIPTING
#include "aver/scripting/ScriptHost.hpp"
#endif

// AverSR (docs/AVERSR.md): Sandbox is the composition root that links Aver.Render.Sr and constructs
// the concrete aver::sr::SpatialUpscaler; the generic RHI (aver/rhi/RHI.hpp) never does and never will.
#if AVER_MODULE_SR
#include "aver/sr/AverSrQuality.hpp"
#include "aver/sr/AverSrSpatial.hpp"
#include "aver/sr/AverSrFxaa.hpp"
// optimisation-wave-2, U2/3.2: render.voxi's ladder constants (via ProjectRenderApply.hpp) mirror
// aver::sr::Quality's numbering on purpose, so Scalability.hpp's resolveAverSrLevel can hand back a
// plain u32 with no translation table of its own. render.voxi never includes aver/sr (module-boundary
// rule), so this is the one place that includes both enums to check they still agree, at compile time
// rather than as a level silently rendering at the wrong scale. Guarded on AVER_MODULE_VOXI because
// the constants are render.voxi's; SR-on/VOXI-off is a real config, and not rare -- PBR=OFF forces
// VOXI=OFF too (root CMakeLists.txt: "Voxi renders materials and cannot be built without them"),
// which is why module-matrix.ps1's pbr-off and voxi-off rows both died on `aver::voxi::ladder` here.
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

// Frame interpolation: Sandbox is the composition root for the IFrameInterpolator, as for AverSR.
#include "aver/neurafi/NeuraFI.hpp"

// Guarded on AVER_MODULE_PHYSICS alone, not FRAMEWORK: SCENE=OFF can force FRAMEWORK off while
// PHYSICS stays on, breaking the guarded call sites below (which check PHYSICS alone) with no
// missing-symbol error to point at it.
#if AVER_MODULE_PHYSICS
#include "aver/physics/physics_abi.h"
#endif
#if AVER_MODULE_FRAMEWORK
#include "aver/framework/framework_abi.h"
#include "aver/framework/framework_hooks.h"
#endif

// Gated on _WIN32 alone, not SCENE: the Win32 calls that need it (applyMcpCommand) have no scene
// dependency.
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
// The placement -> entity loop, shared with the game runtime. See modules/world/README.md.
#include "aver/world/LevelInstance.hpp"
// Divergence census both hosts print -- of what the level loaded, not a frame comparison; see
// SceneCensus.hpp.
#include "aver/world/SceneCensus.hpp"
// Opt-in chunk streaming (SandboxApp::setChunkStreamingEnabled) needs no separate ChunkWorld.hpp
// include -- GameStreaming.hpp above already pulls it in under AVER_MODULE_SCENE.
// Level SCATTER records -> generator palette, shared with the game runtime so the two can't diverge.
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
// Seam + concrete Dear ImGui backend. Needed here, not in the RHI (which must not know ImGui
// exists) -- installing a backend is an app decision.
#include "aver/rhi/d3d12/UiBackend.hpp"
#include "aver/rhi/d3d12/ImGuiUiBackend.hpp"
#endif
#if AVER_WITH_IMGUI_VULKAN
// Same pair for Vulkan, its own macro: a tree can build either backend, both or neither, so
// AVER_WITH_IMGUI means "D3D12 backend types are here" only.
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

// Editor's window-event sink; a free function because Window::setEventCallback takes a plain fn
// pointer + void* (same shape as GameApp's onWindowEvent). Filters nothing -- whether a consumer may
// act on an event is a per-frame policy question answered further down, not here.
static inline void sandboxWindowEvent(void* user, const Event& e) {
    static_cast<InputState*>(user)->onEvent(e);
}

#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
namespace {

// Aver.Save takes function pointers rather than linking Aver.Framework, so this is where the two
// meet. Same shape as the animation notify sink and the animation-curve provider above.

// SPAWNS WITHOUT BeginPlay, and that is the whole reason aver_fw_dispatch_begin_play exists.
// aver_fw_spawn runs bind -> build_models -> beginPlay inline, so an actor spawned and THEN patched
// has already begun play against its class defaults. Preview, patch, then begin.
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

// GRAPH-LOCAL VARIABLES: thin forwards to the aver_fw_graph_var_* relay, the same shape
// saveClassOf/saveBeginPlay/saveDestroyActor use. AVER_SCENE_KIND_F32/I32/BOOL cross unchanged --
// both sides agree on those values (scene_abi.h), the same contract OcSave.hpp's `kind` documents.
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

inline aver::save::Host saveHost() {
    aver::save::Host h;
    h.spawnClass    = &saveSpawnClass;
    h.classOf       = &saveClassOf;
    h.beginPlay     = &saveBeginPlay;
    h.destroyActor  = &saveDestroyActor;
    h.graphVarCount = &saveGraphVarCount;
    h.graphVarAt    = &saveGraphVarAt;
    h.graphVarSet   = &saveGraphVarSet;
    return h;
}

inline i32 saveWriteProvider(const char* path, void*) {
    aver::save::CaptureOptions co;
    co.host = saveHost();
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

inline i32 saveLoadProvider(const char* path, void*) {
    aver::fmt::OcSaveData snap;
    std::string why;
    if (!aver::fmt::loadOcSave(path, snap, &why)) {
        AVER_ERROR("[Save] load failed: {}", why);
        return 0;
    }
    aver::save::RestoreOptions ro;
    ro.host = saveHost();
    if (!aver::save::restore(snap, aver::scene::World::instance(), ro, &why)) {
        AVER_ERROR("[Save] restore failed: {}", why);
        return 0;
    }
    return 1;
}

} // namespace
#endif


// Gizmo axis basis and colours: X red, Y green, Z blue, yellow highlight -- Unreal 5's own gizmo
// palette, sRGB (202,38,0) / (103,169,0) / (44,126,237) / (255,255,0). Drawn with setLineWidth now,
// not setLineGlow (RHI.hpp: the API is gone -- lines draw after the tonemap, in display colour, so
// there is no bloom left to fake a glow from).
static const Vec3 kAxisDir[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
static const Vec3 kAxisCol[3] = {{202.0f/255.0f, 38.0f/255.0f, 0.0f}, {103.0f/255.0f, 169.0f/255.0f, 0.0f},
                                  {44.0f/255.0f, 126.0f/255.0f, 237.0f/255.0f}};
static const Vec3 kAxisHi = {1.0f, 1.0f, 0.0f};

// Appends a cube centred at (cx,cy,cz) with half-extent h.
static inline void appendBox(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx, f32 cx, f32 cy, f32 cz, f32 h) {
    const f32 p[8][3] = {{-h,-h,-h},{h,-h,-h},{h,h,-h},{-h,h,-h},{-h,-h,h},{h,-h,h},{h,h,h},{-h,h,h}};
    struct Face { f32 n[3]; int c[4]; };
    const Face faces[6] = {{{1,0,0},{1,2,6,5}},{{-1,0,0},{0,4,7,3}},{{0,1,0},{3,7,6,2}},
                           {{0,-1,0},{0,1,5,4}},{{0,0,1},{4,5,6,7}},{{0,0,-1},{0,3,2,1}}};
    const f32 quadUV[4][2] = {{0,0},{1,0},{1,1},{0,1}};
    for (const Face& f : faces) {
        const u32 b = static_cast<u32>(v.size());
        for (int k = 0; k < 4; ++k) { const f32* c = p[f.c[k]]; v.push_back({cx+c[0],cy+c[1],cz+c[2],f.n[0],f.n[1],f.n[2],quadUV[k][0],quadUV[k][1]}); }
        idx.push_back(b); idx.push_back(b+1); idx.push_back(b+2); idx.push_back(b); idx.push_back(b+2); idx.push_back(b+3);
    }
}
// Appends a ground quad in the XY plane with half-extent s.
static inline void appendGround(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx, f32 s) {
    const u32 b = static_cast<u32>(v.size());
    v.push_back({-s,-s,0,0,0,1,-0.5f,-0.5f}); v.push_back({s,-s,0,0,0,1,0.5f,-0.5f});
    v.push_back({s,s,0,0,0,1,0.5f,0.5f}); v.push_back({-s,s,0,0,0,1,-0.5f,0.5f});
    idx.push_back(b); idx.push_back(b+1); idx.push_back(b+2); idx.push_back(b); idx.push_back(b+2); idx.push_back(b+3);
}
// Appends a UV sphere of radius r with +Z as the pole.
static inline void appendSphere(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx, f32 r, u32 rings, u32 sectors) {
    const u32 base = static_cast<u32>(v.size());
    for (u32 ring = 0; ring <= rings; ++ring) {
        const f32 phi = kPi * (static_cast<f32>(ring) / static_cast<f32>(rings));
        const f32 z = std::cos(phi), rad = std::sin(phi);
        for (u32 sec = 0; sec <= sectors; ++sec) {
            const f32 theta = 2.0f * kPi * (static_cast<f32>(sec) / static_cast<f32>(sectors));
            const f32 nx = rad * std::cos(theta), ny = rad * std::sin(theta), nz = z;
            v.push_back({nx * r, ny * r, nz * r, nx, ny, nz,
                         static_cast<f32>(sec) / static_cast<f32>(sectors),
                         static_cast<f32>(ring) / static_cast<f32>(rings)});
        }
    }
    const u32 stride = sectors + 1;
    for (u32 ring = 0; ring < rings; ++ring) {
        for (u32 sec = 0; sec < sectors; ++sec) {
            const u32 a = base + ring * stride + sec, b = a + stride;
            idx.push_back(a); idx.push_back(b); idx.push_back(a + 1);
            idx.push_back(a + 1); idx.push_back(b); idx.push_back(b + 1);
        }
    }
}

// Generalised appendBox: independent half-extents (hx,hy,hz), yawed by yawDeg around Z, for the
// drone's arms/body/skids. Face table copied verbatim; yawDeg=0, hx=hy=hz reproduces appendBox.
static inline void appendBoxYaw(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx,
                          f32 cx, f32 cy, f32 cz, f32 hx, f32 hy, f32 hz, f32 yawDeg) {
    const f32 rad = yawDeg * kDegToRad;
    const f32 cs = std::cos(rad), sn = std::sin(rad);
    // Rotates a local (lx,ly,lz) around Z; determinant +1 preserves winding/handedness, so faces
    // stay CCW-outward as in appendBox.
    auto rotZ = [cs, sn](f32 lx, f32 ly, f32 lz, f32& ox, f32& oy, f32& oz) {
        ox = lx * cs - ly * sn; oy = lx * sn + ly * cs; oz = lz;
    };
    const f32 p[8][3] = {{-hx,-hy,-hz},{hx,-hy,-hz},{hx,hy,-hz},{-hx,hy,-hz},
                         {-hx,-hy,hz},{hx,-hy,hz},{hx,hy,hz},{-hx,hy,hz}};
    struct Face { f32 n[3]; int c[4]; };
    const Face faces[6] = {{{1,0,0},{1,2,6,5}},{{-1,0,0},{0,4,7,3}},{{0,1,0},{3,7,6,2}},
                           {{0,-1,0},{0,1,5,4}},{{0,0,1},{4,5,6,7}},{{0,0,-1},{0,3,2,1}}};
    const f32 quadUV[4][2] = {{0,0},{1,0},{1,1},{0,1}};
    for (const Face& f : faces) {
        f32 nx = 0.0f, ny = 0.0f, nz = 0.0f; rotZ(f.n[0], f.n[1], f.n[2], nx, ny, nz);
        const u32 b = static_cast<u32>(v.size());
        for (int k = 0; k < 4; ++k) {
            const f32* c = p[f.c[k]];
            f32 wx = 0.0f, wy = 0.0f, wz = 0.0f; rotZ(c[0], c[1], c[2], wx, wy, wz);
            v.push_back({cx+wx, cy+wy, cz+wz, nx, ny, nz, quadUV[k][0], quadUV[k][1]});
        }
        idx.push_back(b); idx.push_back(b+1); idx.push_back(b+2);
        idx.push_back(b); idx.push_back(b+2); idx.push_back(b+3);
    }
}

// Capped cylinder along +Z (drone motor pods/rotor discs). Flat-shaded per face like appendBox, not
// smooth like appendSphere -- at rotor-pod segment counts (8-10) smoothing wouldn't be visible anyway.
static inline void appendCylinderZ(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx,
                             f32 cx, f32 cy, f32 cz, f32 radius, f32 halfHeight, u32 segments) {
    for (u32 s = 0; s < segments; ++s) {
        const f32 a0 = kTwoPi * static_cast<f32>(s) / static_cast<f32>(segments);
        const f32 a1 = kTwoPi * static_cast<f32>(s + 1) / static_cast<f32>(segments);
        const f32 x0 = std::cos(a0), y0 = std::sin(a0);
        const f32 x1 = std::cos(a1), y1 = std::sin(a1);
        // Side quad: both edges share one flat normal (averaged, renormalised radial direction) --
        // same "one normal per face" rule as appendBox, computed here since it depends on the segment.
        f32 nx = x0 + x1, ny = y0 + y1;
        const f32 nl = std::sqrt(nx * nx + ny * ny);
        if (nl > 1e-6f) { nx /= nl; ny /= nl; }
        const u32 b = static_cast<u32>(v.size());
        v.push_back({cx + x0 * radius, cy + y0 * radius, cz - halfHeight, nx, ny, 0, 0, 0});
        v.push_back({cx + x1 * radius, cy + y1 * radius, cz - halfHeight, nx, ny, 0, 1, 0});
        v.push_back({cx + x1 * radius, cy + y1 * radius, cz + halfHeight, nx, ny, 0, 1, 1});
        v.push_back({cx + x0 * radius, cy + y0 * radius, cz + halfHeight, nx, ny, 0, 0, 1});
        idx.push_back(b); idx.push_back(b+1); idx.push_back(b+2);
        idx.push_back(b); idx.push_back(b+2); idx.push_back(b+3);
        // Top/bottom caps: one fan triangle per segment wedge -- cheap at these segment counts (8-10
        // still reads round) and keeps caps flat-shaded without a third normal convention.
        const u32 ct = static_cast<u32>(v.size());
        v.push_back({cx, cy, cz + halfHeight, 0, 0, 1, 0.5f, 0.5f});
        v.push_back({cx + x0 * radius, cy + y0 * radius, cz + halfHeight, 0, 0, 1, x0*0.5f+0.5f, y0*0.5f+0.5f});
        v.push_back({cx + x1 * radius, cy + y1 * radius, cz + halfHeight, 0, 0, 1, x1*0.5f+0.5f, y1*0.5f+0.5f});
        idx.push_back(ct); idx.push_back(ct+1); idx.push_back(ct+2);
        const u32 cb = static_cast<u32>(v.size());
        v.push_back({cx, cy, cz - halfHeight, 0, 0, -1, 0.5f, 0.5f});
        v.push_back({cx + x1 * radius, cy + y1 * radius, cz - halfHeight, 0, 0, -1, x1*0.5f+0.5f, y1*0.5f+0.5f});
        v.push_back({cx + x0 * radius, cy + y0 * radius, cz - halfHeight, 0, 0, -1, x0*0.5f+0.5f, y0*0.5f+0.5f});
        idx.push_back(cb); idx.push_back(cb+1); idx.push_back(cb+2);
    }
}

// Placeholder quadcopter (420 tris), from appendBoxYaw/appendCylinderZ; replaces the old bare-unit-cube
// drone (kDroneAsset at setDroneEnabled). ANISOTROPIC unlike the unit cube/sphere: 1.0 is reached only
// at the rotor-tip diagonals (kArmROuter + kDiscRadius = 0.80 + 0.20 = 1.00); per-axis reach is ~0.77
// X/Y, ~0.16 up, ~0.22 down. Duplicated vertex-for-vertex in GameContent.cpp (editor vs runtime TUs).
static inline void appendDrone(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx) {
    // Central body: a squarish box, flatter than it is wide -- real quadcopter chassis proportions.
    constexpr f32 kBodyHX = 0.26f, kBodyHY = 0.26f, kBodyHZ = 0.15f;
    appendBoxYaw(v, idx, 0, 0, 0, kBodyHX, kBodyHY, kBodyHZ, 0.0f);

    // Four arms, out to the corners (45/135/225/315 degrees), each ending in a motor pod and a rotor
    // disc -- see this function's own header comment for why kArmROuter + kDiscRadius is exactly 1.0.
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
        // The rotor disc stands in for the swept area of a spinning prop that a static placeholder
        // mesh cannot animate -- a flat approximation, stated here rather than left for someone to
        // wonder why a "propeller" never turns.
        appendCylinderZ(v, idx, hubX, hubY, kDiscCenterZ, kDiscRadius, kDiscHalfHeight, 10);
    }

    // A pair of landing skids plus the four short struts that stand them off the body's underside.
    constexpr f32 kSkidHalfLen = 0.30f, kSkidHalfWidth = 0.02f, kSkidHalfThick = 0.018f;
    constexpr f32 kSkidY = 0.20f, kSkidZ = -0.20f;
    appendBoxYaw(v, idx, 0.0f,  kSkidY, kSkidZ, kSkidHalfLen, kSkidHalfWidth, kSkidHalfThick, 0.0f);
    appendBoxYaw(v, idx, 0.0f, -kSkidY, kSkidZ, kSkidHalfLen, kSkidHalfWidth, kSkidHalfThick, 0.0f);

    constexpr f32 kStrutHalfX = 0.02f, kStrutHalfY = 0.02f, kStrutHalfZ = 0.016f;
    // Midpoint between the body's underside (-kBodyHZ = -0.15) and the skid's top (kSkidZ +
    // kSkidHalfThick = -0.182).
    constexpr f32 kStrutX = 0.16f, kStrutZ = -0.166f;
    for (f32 sx : {-kStrutX, kStrutX})
        for (f32 sy : {-kSkidY, kSkidY})
            appendBoxYaw(v, idx, sx, sy, kStrutZ, kStrutHalfX, kStrutHalfY, kStrutHalfZ, 0.0f);
}

// Euler<->quaternion pair now lives in EditorEuler.hpp, testable -- was two static functions here,
// unreachable from any test. eulerDegFromQuat's gimbal branch was wrong in a way that wrote corrupted
// rotations into saved levels.
using aver::editor::quatFromEulerDeg;
using aver::editor::eulerDegFromQuat;

// Rounds v to the nearest multiple of step; returns v unchanged when step is zero.
static inline f32 snapf(f32 v, f32 step) { return step > 0.0f ? std::round(v / step) * step : v; }

// Transforms a point by the row-vector matrix m.
static inline Vec3 xformPoint(const Mat4& m, const Vec3& p) {
    return { p.x*m.m[0][0]+p.y*m.m[1][0]+p.z*m.m[2][0]+m.m[3][0],
             p.x*m.m[0][1]+p.y*m.m[1][1]+p.z*m.m[2][1]+m.m[3][1],
             p.x*m.m[0][2]+p.y*m.m[1][2]+p.z*m.m[2][2]+m.m[3][2] };
}
// Transforms a direction by the row-vector matrix m, ignoring translation.
static inline Vec3 xformVec(const Mat4& m, const Vec3& v) {
    return { v.x*m.m[0][0]+v.y*m.m[1][0]+v.z*m.m[2][0],
             v.x*m.m[0][1]+v.y*m.m[1][1]+v.z*m.m[2][1],
             v.x*m.m[0][2]+v.y*m.m[1][2]+v.z*m.m[2][2] };
}
// Intersects a ray with an AABB. Returns true and writes the entry distance to tHit.
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
// Converts an interleaved MeshVertex/index pair into a PickGeometry, for the file's three built-in
// meshes only (sphere/cube/drone). Project meshes use pickGeometryFor instead, straight from
// fmt::OcMeshData's separate position/normal arrays -- no interleave/un-interleave round trip.
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
// Builds the floor grid line list out to extent ext with the given cell step.
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

// Appends one coloured line segment.
static inline void gzLine(std::vector<rhi::LineVertex>& v, const Vec3& a, const Vec3& b, const Vec3& c) {
    v.push_back({a.x,a.y,a.z, c.x,c.y,c.z}); v.push_back({b.x,b.y,b.z, c.x,c.y,c.z});
}
// Builds the unit-length translate arrow for axis a.
static inline std::vector<rhi::LineVertex> buildMoveAxis(int a, const Vec3& c) {
    std::vector<rhi::LineVertex> v;
    const Vec3 A = kAxisDir[a], P = kAxisDir[(a+1)%3], Q = kAxisDir[(a+2)%3];
    gzLine(v, {0,0,0}, A, c);
    const Vec3 tip = A, base = A * 0.80f;
    for (int k = 0; k < 4; ++k) { f32 t = k * (kPi * 0.5f); Vec3 r = P*(std::cos(t)*0.07f) + Q*(std::sin(t)*0.07f); gzLine(v, base+r, tip, c); }
    return v;
}
// Builds the unit rotation ring perpendicular to axis a.
static inline std::vector<rhi::LineVertex> buildRotRing(int a, const Vec3& c) {
    std::vector<rhi::LineVertex> v;
    const Vec3 P = kAxisDir[(a+1)%3], Q = kAxisDir[(a+2)%3];
    const int N = 64; Vec3 prev{};
    for (int k = 0; k <= N; ++k) { f32 t = k * (kTwoPi / N); Vec3 p = P*std::cos(t) + Q*std::sin(t); if (k > 0) gzLine(v, prev, p, c); prev = p; }
    return v;
}
// Builds the unit scale handle for axis a: a shaft with a box at the tip.
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

// Builds the Player Start's wire capsule, standing on the local origin (the spawn point is the
// pawn's FEET) -- Unreal's own capsule-component visualiser: two rings at the hemisphere seams, four
// vertical side lines between them, and a half-circle arc over each pole in the XZ and YZ planes.
// Local space, no yaw baked in: the caller's world matrix supplies the Player Start's own rotation.
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
    // Half circles: theta 0 sits on the ring (x=radius), theta pi/2 at the pole, theta pi on the
    // ring's far side -- a full 180-degree arc through the dome, one in each vertical plane.
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
// Builds the Player Start's facing arrow: a shaft along local +X (yawDeg 0's own forward -- see
// addPlayerStart's atan2(f.y, f.x)) at height centerZ, with a two-line arrowhead. Local space, like
// buildCapsuleWire above -- the caller's world matrix supplies position and yaw.
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

// Select edits OBJECTS (pick+gizmo); Landscape edits TERRAIN (sculpt, no picking/gizmo) -- a mode,
// not a tool (sculpt tools reuse this shape; see handleSculpt()). Previously one enum with
// Raise/Lower/Smooth/Flatten beside Move/Rotate, so gizmo and picking ran during terrain edits with
// no way to tell which activity a click meant. Foliage and Simulate (framework's play/pause/stop,
// AVER_FW_PLAY_*) were added because both were already real. Mesh Paint / Geometry-Modeling absent:
// OcMeshData has no vertex-colour/weight channel to paint yet, and "MeshEditor" is read-only -- an
// empty-panel mode is worse than none.
enum class EditorMode { Select, Landscape, Foliage, Simulate };
static const char* kEditorModeNames[4] = {"Select", "Landscape", "Foliage", "Simulate"};
// One-line "what is this for", shown in the dropdown under each name the way UE's mode picker does.
static const char* kEditorModeHints[4] = {
    "Pick and transform objects",
    "Sculpt the terrain heightfield",
    "Paint scattered meshes onto the terrain",
    "Run the game in the viewport",
};
static constexpr int kEditorModeCount = 4;

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
inline constexpr f32 kPlayerStartIconHalfSize = 24.0f;        // cm
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
// World Outliner's reparent drag. Fixed 4-byte scene::Entity, so every target guards on size
// EQUALITY (kAssetDragDropType's payload is a variable-length path and cannot).
static constexpr const char* kOutlinerReparentDragDropType = "AVER_OUTLINER_ENTITY";

// Dragging assets BETWEEN Content Browser folders (not into the level). A SECOND type, not a reuse
// of kAssetDragDropType: that one is filtered to .ocmesh/.ocparticle for the viewport, but moving
// files has no such restriction -- reusing it would silently drop unsupported types from the move.
// Both payloads are set from the same drag (ImGui allows several types per source); each target
// reads only the type it understands.
static constexpr const char* kCbMoveDragDropType = "AVER_CB_MOVE_SET";

// One placed object in the editor scene: mesh, transform, and surface parameters.
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
// Canonical Aver orange (was GraphEditor.cpp's IM_COL32(242,101,34), drifted into four different
// oranges: 0.95/0.42/0.13 accent (six units off on green), 1.0/0.62/0.12 selection outline,
// 0.79/0.47/0.16 ad-hoc). Selection outline stays deliberately distinct (must read against arbitrary
// scene colour, not chrome); rest unified here.
static constexpr ImVec4 kAverOrange   (242.0f/255.0f, 101.0f/255.0f, 34.0f/255.0f, 1.00f);
static constexpr ImVec4 kAverOrangeDim(242.0f/255.0f, 101.0f/255.0f, 34.0f/255.0f, 0.55f);

// Applies the editor's dark colour scheme and metrics. Colours are set explicitly for all ~50 ImGui
// entries (unset ones read default blue against this orange-on-steel identity), with a slight blue
// bias on neutrals so the accent reads as chosen. Split from the colours function on purpose: the
// only caller, applyDpi, runs ScaleAllSizes(dpi) right after this, so a merged function called alone
// would reset metrics to unscaled and drop the DPI scale -- on a 1.5x display that takes
// FramePadding.y from 6 back to 4, which shifts the docked tab-bar height, the Level viewport rect
// --probe-rel resolves against, and all twenty oracle gates. Metrics apply with the DPI pass; colours
// can apply anytime.
static inline void applyEditorMetrics() {
    ImGuiStyle& s = ImGui::GetStyle();

    // Set UNSCALED -- applyDpi's ScaleAllSizes(dpi) runs right after this, so pre-multiplied values
    // here would square the scaling on a high-DPI display (see function comment above).
    s.WindowRounding    = 4;  s.ChildRounding  = 4;  s.FrameRounding  = 4;
    s.PopupRounding     = 4;  s.GrabRounding   = 3;  s.TabRounding    = 4;
    s.ScrollbarRounding = 4;
    s.WindowBorderSize  = 1;  s.FrameBorderSize = 0; s.PopupBorderSize = 1;
    s.ChildBorderSize   = 1;
    s.WindowPadding     = ImVec2(10, 8);
    // FramePadding.y stays at 4 -- gates constraint: docked tab bar height = FontSize + FramePadding.y*2,
    // and --probe-rel (vpX_/vpY_/vpW_/vpH_) survives a resize but not an aspect change. The x half is free.
    s.FramePadding      = ImVec2(8, 4);
    s.ItemSpacing       = ImVec2(8, 6);
    s.ItemInnerSpacing  = ImVec2(6, 4);
    s.CellPadding       = ImVec2(6, 4);
    s.ScrollbarSize     = 12;
    s.GrabMinSize       = 10;
    s.IndentSpacing     = 18;
    s.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
    s.SeparatorTextBorderSize = 1;
}

// The palette. Safe to re-run at any point outside a widget's own draw, because it writes colours
// and touches no metric -- see applyEditorMetrics above for why that distinction is load-bearing.
static inline void applyEditorColors() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;

    // One ladder of neutrals, deepest to lightest, so depth is one consistent set rather than each
    // widget family inventing its own near-black.
    const ImVec4 sunken   (0.071f, 0.075f, 0.082f, 1.00f);   // behind content: viewports, child frames
    const ImVec4 panel    (0.109f, 0.114f, 0.125f, 1.00f);   // window bodies
    const ImVec4 raised   (0.145f, 0.152f, 0.165f, 1.00f);   // title bars, menu bar, inactive tabs
    const ImVec4 item     (0.180f, 0.188f, 0.204f, 1.00f);   // buttons, frames, headers at rest
    const ImVec4 itemHot  (0.235f, 0.245f, 0.265f, 1.00f);
    const ImVec4 itemOn   (0.275f, 0.287f, 0.310f, 1.00f);
    const ImVec4 line     (0.043f, 0.047f, 0.055f, 1.00f);   // borders and separators
    const ImVec4 text     (0.882f, 0.894f, 0.910f, 1.00f);
    const ImVec4 textDim  (0.478f, 0.494f, 0.522f, 1.00f);

    c[ImGuiCol_Text]                  = text;
    c[ImGuiCol_TextDisabled]          = textDim;
    c[ImGuiCol_WindowBg]              = panel;
    c[ImGuiCol_ChildBg]               = sunken;
    c[ImGuiCol_PopupBg]               = ImVec4(0.094f, 0.098f, 0.110f, 0.98f);
    c[ImGuiCol_Border]                = line;
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_FrameBg]               = item;
    c[ImGuiCol_FrameBgHovered]        = itemHot;
    c[ImGuiCol_FrameBgActive]         = itemOn;

    c[ImGuiCol_TitleBg]               = raised;
    c[ImGuiCol_TitleBgActive]         = raised;
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.071f, 0.075f, 0.082f, 0.85f);
    c[ImGuiCol_MenuBarBg]             = raised;

    // Scrollbars were default blue-grey and are on screen constantly -- easily the most visible of
    // the entries that had been left unset.
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.071f, 0.075f, 0.082f, 0.60f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.235f, 0.245f, 0.265f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.310f, 0.325f, 0.350f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = kAverOrangeDim;

    c[ImGuiCol_CheckMark]             = kAverOrange;
    c[ImGuiCol_SliderGrab]            = kAverOrange;
    c[ImGuiCol_SliderGrabActive]      = ImVec4(1.000f, 0.478f, 0.208f, 1.00f);   // orange, lifted

    c[ImGuiCol_Button]                = item;
    c[ImGuiCol_ButtonHovered]         = itemHot;
    c[ImGuiCol_ButtonActive]          = kAverOrangeDim;

    c[ImGuiCol_Header]                = item;
    c[ImGuiCol_HeaderHovered]         = itemHot;
    c[ImGuiCol_HeaderActive]          = kAverOrangeDim;

    c[ImGuiCol_Separator]             = line;
    c[ImGuiCol_SeparatorHovered]      = kAverOrangeDim;
    c[ImGuiCol_SeparatorActive]       = kAverOrange;

    c[ImGuiCol_ResizeGrip]            = ImVec4(0.235f, 0.245f, 0.265f, 0.60f);
    c[ImGuiCol_ResizeGripHovered]     = kAverOrangeDim;
    c[ImGuiCol_ResizeGripActive]      = kAverOrange;

    // Active tab was ImGui default blue; orange top edge on a panel-coloured body reads as "this
    // one" without shouting (a fully orange tab would).
    c[ImGuiCol_Tab]                   = raised;
    c[ImGuiCol_TabHovered]            = itemHot;
    c[ImGuiCol_TabActive]             = panel;
    c[ImGuiCol_TabUnfocused]          = ImVec4(0.094f, 0.098f, 0.110f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.130f, 0.136f, 0.148f, 1.00f);

    c[ImGuiCol_DockingPreview]        = kAverOrangeDim;
    c[ImGuiCol_DockingEmptyBg]        = sunken;

    c[ImGuiCol_PlotLines]             = ImVec4(0.640f, 0.660f, 0.700f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]      = kAverOrange;
    c[ImGuiCol_PlotHistogram]         = kAverOrange;
    c[ImGuiCol_PlotHistogramHovered]  = ImVec4(1.000f, 0.478f, 0.208f, 1.00f);

    c[ImGuiCol_TableHeaderBg]         = raised;
    c[ImGuiCol_TableBorderStrong]     = line;
    c[ImGuiCol_TableBorderLight]      = ImVec4(0.078f, 0.082f, 0.094f, 1.00f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(1, 1, 1, 0.022f);   // banding, barely there on purpose

    c[ImGuiCol_TextSelectedBg]        = ImVec4(kAverOrange.x, kAverOrange.y, kAverOrange.z, 0.35f);
    c[ImGuiCol_DragDropTarget]        = kAverOrange;
    c[ImGuiCol_NavHighlight]          = kAverOrange;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1, 1, 1, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.071f, 0.075f, 0.082f, 0.60f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.020f, 0.022f, 0.026f, 0.65f);
}
#endif

// One captured log line for the Output Log panel.
struct LogLine { LogLevel level; std::string text; };

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
    // Typed glyph when no sprite art exists (-1 = none, falls back to the anonymous page). Resolved
    // at LISTING time not draw time: the gallery redraws every frame, so this avoids an extension
    // lookup per tile per frame -- the cost the 20-frame cache exists to avoid.
    int kind = -1;
    std::string kindExt;   // lower-cased extension, for assetKindFor at draw time
    // Only meaningful when kindExt == ".ocgraph". Read from the file's DOMAIN record at LISTING time
    // (same reason as `kind` above: avoids re-parsing per tile per frame). Defaults to Gameplay, same
    // as an absent DOMAIN record, so a non-graph entry and a graph this build failed to open both read
    // the same harmless way.
    editor::GraphAssetFamily graphFamily = editor::GraphAssetFamily::Gameplay;
};

// A Content Browser directory listing, refreshed on a frame stamp. Folders sort first and are counted.
struct DirListing { int stamp = -1000; std::vector<DirEntry> entries; usize dirCount = 0; };

// The object animation one placed mesh plays in Play -- what a PLACE record's anim/animspeed/animtime/
// animonce tokens hold. AUTHORED values only: the entity's CAnimator is the live clock and Play advances
// it, so a save reads this and never that. clip is a content-relative .ocanim path with forward slashes
// and the extension; empty = no animation.
struct EntityAnim {
    std::string clip;
    f32  speed = 1.0f;
    f32  time  = 0.0f;     // start time t0, seconds
    bool once  = false;
    bool operator==(const EntityAnim&) const = default;
};

// Replaces the characters Windows refuses in a file name. Lives here, at file scope, rather than as a
// SandboxApp member, because importGltfToDir needs it and must be callable before any SandboxApp exists.
static inline void sanitiseAssetName(std::string& s) {
    for (char& c : s)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    if (s.empty()) s = "unnamed";
}

// How many of each asset kind importGltfToDir actually wrote to destDir, as opposed to skipping
// because a same-named output file was already sitting there.
struct GltfImportSummary { u32 meshesWritten = 0, rigsWritten = 0, clipsWritten = 0,
                                materialsWritten = 0, texturesWritten = 0; };

// Converts a glTF/GLB into one .ocmesh per mesh (+ .ocskel/.ocanim if skinned) into destDir. Free
// function (not a member): shared by Content Browser Import and --import-gltf, before any SandboxApp
// exists. Returns false with *outWhy only on a hard parse failure; a clean parse writing nothing new
// returns true with an all-zero summary -- callers decide if that counts as failure.
// contentDir: project's Content directory, where materials/textures are cooked to (empty = geometry
// only). overwrite: replace existing outputs (each goes to the recycle bin first) instead of skipping.
// Defined in SandboxContentBrowser.cpp.
bool importGltfToDir(const std::string& src, const std::string& destDir, const std::string& contentDir,
                     bool overwrite, GltfImportSummary& out, std::string* outWhy);

// Forward-declared so SandboxApp::handleOpenRequest (single-instance forwarding's accept/decline
// check) can call them; full definitions stay in their natural home above createApplication.
bool isLevelFile(const char* p);
std::string ownerProjectOf(const std::string& mapPath);

// G-BUFFER DEBUG VIEW.
// WHY: gBufferVelocityTexture/gBufferViewZTexture/gBufferNormalRoughnessTexture (RHI.hpp) are SRVs
// nothing else consumes -- an unsampled buffer looks the same whether correct or silently all zero
// (see MEMORY "Unbacked verification claims").
// WHY A NEW IRenderFeature: the G-buffer is generic IDevice state owned by no render-feature module,
// so IRenderFeature::overlayPass (RHIResources.hpp) is the seam built for exactly this.
// WHY overlayPass: runs after the post chain resolves MSAA/tonemap and after the deferred sky, so
// this view can't be silently overwritten by the sky's depth-EQUAL fill.
// SCOPE: output-only, never writes the three textures; forward-declared below, called from init().
static const char* gbufferDebugShaderSource();

class GBufferDebugFeature final : public rhi::IRenderFeature {
public:
    enum class Mode : u32 { Off = 0, Velocity = 1, ViewZ = 2, NormalRoughness = 3 };

    // Texels/frame that saturate the debug pixel. Chosen, not measured: 8 texels/frame is already a
    // brisk pan at edit-viewport res -- why the shader biases by 0.5 instead of a bare multiply.
    static constexpr f32 kVelocityFullScaleTexels = 8.0f;
    // World-space distance (cm) mapped to fully white in the view-depth debug view. Cosmetic only --
    // gBufferViewZTexture's real values are untouched.
    static constexpr f32 kViewZDebugFarUnits = 5000.0f;

    const char* name() const override { return "GBufferDebug"; }

    // Borrowed; the device outlives this feature for the run (same non-ownership every other
    // IRenderFeature in this file already assumes -- see voxiRenderer_'s own member comment).
    void setDevice(rhi::IDevice* dev) { device_ = dev; }
    void setMode(Mode m) { mode_ = m; }
    // 3D viewport rect within the backbuffer (vpX_/vpY_/vpW_/vpH_), not the whole window -- unnarrowed,
    // overlayPass's full-backbuffer viewport would paint over the editor chrome too. 0 = skip this frame.
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

        // x: mode; y: velocity full-scale (texels/frame); z: viewZ debug far distance; w: unused.
        // Kept in sync BY HAND with gbufferDebugShaderSource()'s PSGBufferDebug (no access to Mode enum).
        const f32 cb[4] = { static_cast<f32>(static_cast<u32>(mode_)), kVelocityFullScaleTexels,
                            kViewZDebugFarUnits, 0.0f };
        // Narrowed to the 3D viewport ALONE -- see setViewportRect's own comment for why the rect
        // this method receives by default covers the whole backbuffer, panels included.
        ctx.setViewport(vpX_, vpY_, vpW_, vpH_);
        ctx.setScissor(vpX_, vpY_, vpW_, vpH_);
        ctx.setPipeline(pipeline_);
        ctx.setBindingSet(binding_);
        ctx.setConstantBuffer(kGBufferDebugConstantRegister, cb, sizeof(cb));
        ctx.drawFullscreen();
        // Restored to the full backbuffer: the narrowing must not outlive this one draw call, or the
        // next overlayPass / the editor's own UI would inherit this method's rect instead of its own.
        ctx.setViewport(0, 0, width, height);
        ctx.setScissor(0, 0, width, height);
    }

    // Releases the pipeline/binding set while the device is alive. Idempotent; the destructor calling
    // this is a fallback only -- see onShutdown, where both are torn down explicitly and in order.
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

    // Lazy, like FxaaResolve::ensurePipeline -- same fullscreen-triangle recipe, but against three
    // G-buffer SRVs (not one scene colour) and the backbuffer format directly (already bound).
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
            // RGBA8Unorm, NOT device_->backbufferFormat() -- that name is a trap: it returns
            // kSceneColorFormat (RGBA16F HDR scene target), not what overlayPass draws onto. Trusting
            // it once produced 12 debug-layer errors/frame then device removal: "render target format
            // in slot 0 does not match ... (pipeline state = R16G16B16A16_FLOAT, render target format
            // = R8G8B8A8_UNORM, RTV = 'Viewport.Composite')".
            // overlayPass draws onto the tonemapped 8-bit composite, hence the hardcode (same as
            // UiRenderer.cpp:105). Real fix: rename backbufferFormat() to sceneColorFormat(), RHI-wide.
            pd.renderTargets[0] = rhi::Format::RGBA8Unorm;
            pd.sampleCount = 1;   // overlayPass runs on the already-resolved, single-sample composite
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

    // Own root CBV register: no rhi::sharedShaderPrelude() here, so b1 is unrelated to what b1 means
    // to a pipeline that does -- same reasoning as kFxaaConstantRegister (AverSrFxaa.cpp).
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

// HLSL behind GBufferDebugFeature -- standalone fullscreen-triangle pass, own root signature (no
// rhi::sharedShaderPrelude(), same as AverSrFxaa/AverSrSpatial: reads no other pipeline's constants).
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

// NEURAFI VISUALISATION OVERLAY (Window > Neural Visualiser).
// NeuraFI writes its visualisation image in the gather (neurafi.hlsl's vizColour, display-ready
// colours); this draws it over the 3D viewport in overlayPass -- after the tonemap, on BOTH presented
// images of an interpolated frame -- alpha-blended at the chosen opacity (shaders/neural_visualiser.hlsl).
// Drawn only when NeuraFI wrote a fresh image this frame (vizCount moved since onUpdate's baseline), so a
// paused interpolation never leaves a frozen picture on screen. Same lifetime rules as
// GBufferDebugFeature: registered at init, unregistered and shut down explicitly at onShutdown.
class NeuraFiVizFeature final : public rhi::IRenderFeature {
public:
    const char* name() const override { return "NeuraFiViz"; }
    ~NeuraFiVizFeature() override { releaseGpu(); }

    void setDevice(rhi::IDevice* dev) { device_ = dev; }
    // Called once per frame from onUpdate, before the frame renders: the source (null = off), its
    // vizCount() now (so overlayPass draws only an image written after this), and the opacity.
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
            pd.renderTargets[0] = rhi::Format::RGBA8Unorm;   // the tonemapped composite (see GBufferDebugFeature)
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

    static constexpr u32 kConstantRegister = 1;   // own b1, no shared prelude (as GBufferDebugFeature)

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

#if AVER_WITH_IMGUI || AVER_WITH_IMGUI_VULKAN
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

    // ---- content and mesh loading ----
    // Content index, asset resolvers, mesh registry, material cache live in content_
    // (aver::game::GameContent), shared with the runtime. Here: pick triangles/tri counts, the LOD
    // ladder and cluster data (see onMeshLoaded), the picker's eager material preload, reload wrappers.

    // Uploads the project's meshes through content_, then builds the editor's own per-mesh tables in
    // onMeshLoaded.
    void loadProjectMeshes(Engine& e);
#if AVER_MODULE_SCENE
    // One loadProjectMeshes or registerBuiltins call's worth of what onMeshLoaded needs: the app, the
    // engine it uploads through, and the LOD ladder totals loadProjectMeshes reports once at the end.
    struct MeshLoadPass {
        SandboxApp* app = nullptr;
        Engine* engine = nullptr;
        u32 lodCoarserLevels = 0;
        u32 lodSharedLevels = 0;
        u64 lodSharedVertexBytesSaved = 0;
    };
    // content_'s per-mesh hook: pick geometry, triangle counts, skinned ids, and (Trifactor) the LOD
    // ladder, depth proxies and cluster data for each mesh content_ uploads. `user` is a MeshLoadPass.
    static void onMeshLoaded(const game::GameContent::LoadedMesh& mesh, void* user);
#endif

#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    void ensureLodMeshPipeline(Engine& e);
#endif

    void releaseProjectMeshes(Engine& e);

    // Loads every .ocmat under Binaries/Materials AND Content/Materials, binding each to the surface
    // token its stem interns to -- same two homes/precedence as materialForSurface(). Binaries wins
    // a name collision (materialForSurface() tries binariesDir first). Non-recursive in each
    // directory, matching materialForSurface().
#if AVER_MODULE_PBR
    void loadProjectMaterials();

    void releaseProjectMaterials();

    // ---- level-scoped material residency (docs: PACKAGE level-materials) ----
    //
    // True when the editor keeps only the OPEN LEVEL's materials (and their textures) resident
    // instead of every material under the project's Content/Binaries Materials folders -- the
    // default. A/B switch for the change this enables: AVER_LEVEL_SCOPED_MATERIALS=0 in the
    // environment restores the old always-everything-resident behaviour (loadProjectMaterials()
    // called from applyProject, exactly as before this existed). No console var yet -- EditorConsole.hpp
    // is not owned by this change; see this package's final report for the exact line a
    // console slot would add.
    //
    // THE RELEASE HALF IS THE ONLY HALF THIS APP OWNS: unloadLevel() (SandboxLevelLoad.cpp) checks
    // this and, when true, releases every non-pinned resident material before the next level even
    // starts parsing. The BIND half needs no new code here at all -- GameLevel::load() (shared with
    // the runtime) already resolves every surface a level actually draws through
    // materialForSurface()/bindSurfaceMaterial() as it places entities (placement overrides via
    // opt.bindMaterial, and every placed mesh's own material slots -- see that function's own
    // comment in Runtime/src/GameLevel.cpp), which re-loads anything this just released the moment
    // the level that needs it opens.
    static bool levelScopedMaterialsEnabled();

    // Every surface name that must survive a level-scoped release regardless of whether the
    // INCOMING level needs it -- an editor tab or a selection holding a pbr::MaterialHandle open for
    // editing, which must not go dead under it mid-edit. Empty by default: nothing in this package's
    // owned files opens such a tab (the Details panel edits a handle it re-resolves per frame from
    // the CURRENT level's own surfaces, which this mechanism never releases while that level stays
    // open -- see applyMaterialDesc's own comment). A non-owned material/asset editor tab that keeps
    // a handle open ACROSS a level change should call pinMaterialResident() when it opens a material
    // and unpinMaterialResident() when it closes; see this package's final report for exactly where.
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
    void setVSyncOn(bool on) { vsyncOnRequested_ = on; }   // --vsync
    void setLodSelect(bool on, f32 thresholdPx);
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
    // --drone-graph <relPath>. `droneGraphRel_` is guarded `#if AVER_MODULE_SCENE` at its
    // declaration, so this setter must match -- same pre-existing scoping bug as
    // setFogMatchToStreamRadius below, fixed while verifying this task's own -DAVER_MODULE_SCENE=OFF
    // build; not part of the LOD-select/mesh-cluster work, flagged separately in this task's report.
#if AVER_MODULE_SCENE
    void setDroneGraph(std::string relPath);
#else
    void setDroneGraph(std::string);
#endif

    void setLandscapePath(std::string path);

    // --fog-match. Negative opacity means "leave the target where it is", just switch matching on.
    // PRE-EXISTING SCOPING BUG, fixed in passing while verifying this task's own
    // -DAVER_MODULE_SCENE=OFF build: matchFogToStreamRadius_/fogMatchTargetOpacity_ are guarded
    // #if AVER_MODULE_SCENE at their declaration (unlike setChunkStreamAuto/setDroneAuto above), so
    // it simply failed to compile with the module off -- this setter must match too.
#if AVER_MODULE_SCENE
    void setFogMatchToStreamRadius(bool on, f32 targetOpacity);
#else
    void setFogMatchToStreamRadius(bool, f32);
#endif
    void setDroneAuto(int framesIn);
    void setUndoTestAuto(int framesIn);
    void setKeybindTestAuto(std::string mode, int framesIn);
    void setShowEditorPrefs(bool on);   // --editor-prefs
    void setScrollPrefsToKeybinds(bool on);
    void setHudTest(int idx);   // --hud-preview <index>
    void setSaveProject(bool on);   // --save-project
    void setSaveLevelTo(std::string p);   // --save-level <out>
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
    void setPtSceneToggleOnAuto(int framesIn);
    void setPtSceneToggleOffAuto(int framesIn);
    void setSunSetAt(int framesIn, f32 elevDeg, f32 azimDeg);
    void setSunSweep(int framesIn, f32 degPerFrame, int turns);
    void setGiHistoryResetAt(int framesIn);
#if AVER_MODULE_SR
    void setAverSrCycleAuto(int framesIn);   // --aversr-cycle [N]
#endif
    void setResizeCycle(int n);              // --resize-cycle [N]
    void setFrameInterpCli(int on) { frameInterpCli_ = on; }   // --frame-interp 0|1|2 (2: capture the generated image)
    void setFrameInterpTrajectory(int t, bool train) { frameInterpTrajectoryCli_ = t; frameInterpTrainCli_ = train; }
    // --neurafi-view, --neurafi-generated-only, --neurac-view, --neurac-grid (negative = not given).
    void setNeuralVisualiserCli(int neurafiView, bool generatedOnly, int neuracView, bool neuracGrid) {
        if (neurafiView >= 0) neurafiVizMode_ = neurafiView;
        if (generatedOnly) neurafiShowGeneratedOnly_ = true;
        if (neuracView >= 0) neuracViewMode_ = neuracView;
        if (neuracGrid) neuracViewGrid_ = true;
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
    // ---- THREE THINGS THAT ARE NOT NAVIGATION ----
    // A physics collider overlay, a GPU profiler panel and the Content Browser's References panel
    // were declared under `#if AVER_MODULE_SYNAPSE`, unrelated to nav, and it cost nothing only
    // because Synapse has no option() of its own and is always compiled in -- exactly the risk
    // scripts/module-matrix.ps1 warns about ("a module absent from this list is a module nobody
    // checks"). The day Synapse gets a switch, every one of these breaks at once. Moved under the
    // guards they actually need instead.
#if AVER_MODULE_PHYSICS
    // Rebuilds the collider overlay's two line meshes (static bodies; bodies that move): the world-space
    // AABB of every physics body. Called every frame the toggle is on; returns at once on a frame
    // nothing that draws could have changed.
    void rebuildColliderOverlay(Engine& e);
#endif
#if AVER_WITH_IMGUI
    void buildReferencesPanel();

    void buildProfilerPanel(Engine& e);
    // Window > Neural Visualiser: NeuraFI's and NeuRaC's visualisations (SandboxShell.cpp).
    void buildNeuralVisualiserPanel(Engine& e);
#endif
    // Restarts what the profiler panel measures: the device's since-boot GPU average always, and the
    // CPU phase table too when Play is starting (Stop leaves the last session's phases readable).
    // Outside the ImGui guard because startPlay/stopPlay call it in every build.
    void resetPlayProfile(bool playStarting);

#if AVER_MODULE_SYNAPSE
    void setBakeNavOnStart(f32 cellCm);   // --bake-nav [cm]

    // Rebuilds the overlay line mesh from nav_. Destroying the old one FIRST is the point -- runs on
    // every bake/level open; without it each call leaked one committed upload buffer.
    void rebuildNavOverlay(Engine& e);

    // Paired with the definition's own guard (SandboxShell.cpp): AVER_MODULE_SYNAPSE is grid math
    // alone, proves no world to sample -- a declaration visible on SYNAPSE alone would link to nothing.
#if AVER_MODULE_SCENE
    bool bakeNavigationNow(Engine& e, std::string* why = nullptr);
#endif

    void loadNavForLevel(Engine& e);
#endif
    void setFocusCompile(bool b);   // --compile-scripts
    void setFocusReload(int frames); // --reload-scripts [N]
    void warnDeadMaterialHandle(i32 mat);

    // F2: THE ONE RESOLVER, replacing three copies (entity loop, submitShadowOnly, drawMeshParts --
    // all since removed/merged into planEntityDraws + game::drawWorld) that each ran surfaceMaterials_
    // .find/MaterialLibrary::desc/isTranslucent/surfaceLooks_.find/the MaterialSystem lookup BY HAND.
    // One copy had drifted: it never checked handle liveness, so a dead handle baked in the
    // bright-white-mirror fallback as final (see resolveSurfaceLook in aver/game/SceneSubmission.hpp).
    // `mat` is the material TOKEN, already carrying whatever entity-/part-level meshDefaultMaterial or
    // part-slot fallback the caller resolved -- this only resolves what the token means, never
    // re-derives the fallback itself.
    struct ResolvedSurface {
        aver::game::SurfaceLook look;
        u32 authored = 0;                    // pbr::MaterialLibrary handle, or 0 (built-in look/fallback)
        rhi::BindingSetHandle matSet = 0;
        const void* matConstants = nullptr;   // reference into MaterialSystem's storage; see resolveSurface's contract
        u32 matBytes = 0;
    };

    // Still here though game::drawWorld resolves every draw itself: two readers this library can't
    // serve remain -- the GPU cluster mesh-shader path binds material through the RENDER CONTEXT, not
    // the device (setDrawBinding only forwards from inside drawMesh(), which that path skips) and
    // gates itself on the resolved look's `blended`, and DrawWorldOptions::onSurfaceWarn re-runs this
    // so the editor's once-per-token warnings stay editor-specific. Both resolvers share the call to
    // aver::game::resolveSurfaceLook -- the half that drifted.
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

    void applyUpscalerSlot(rhi::IDevice* dev);

    void ensureEdgeAaUpscaler(rhi::IDevice* dev);

    void logAverSrActive(rhi::IDevice* dev);

    void applyAverSrQuality(rhi::IDevice* dev, aver::sr::Quality q);

    // THE THREE THAT NEED VOXI AS WELL AS SR, nested separately: each names a render.voxi type
    // outright (AverSrSource, voxi::Settings, voxi::DeviceInfo) because the CLI > user > manifest >
    // ladder chain they resolve is the ladder's, and the ladder is Voxi's. Everything above is AverSR
    // alone and keeps working with the renderer compiled out. onUpdate's call site already asks for
    // both (SandboxApp.cpp); this declaration now agrees with it.
#if AVER_MODULE_VOXI
    const char* averSrSourceText(voxi::AverSrSource source) const;

    const char* averSrAutoRungName(const voxi::Settings& s, const voxi::DeviceInfo& d) const;

    void updateAverSrAuto(Engine& e);
#endif  // AVER_MODULE_VOXI
#endif  // AVER_MODULE_SR

    void syncPtSceneView(rhi::IDevice* dev);

    void setFrameTimeReport(bool on);                 // --frame-time
    void setRayDrivenAblation(int m);
    void setRtDenoiseMotionTaper(f32 v);
    void setRefractionOverrides(int mode, f32 strength, f32 fade);
    void setMsOverride(bool on);                           // --ms
    void setProbe(u32 x, u32 y);                    // --probe X Y
    void setProbeRel(f32 u, f32 v);                 // --probe-rel U V
    void setCamWobble(f32 degrees, i32 periodFrames);
    // --cam-wobble-stop N: stop the wobble at frame N, so a capture at N+k measures k frames after
    // the camera stopped. --set NAME VALUE (repeatable): console variables applied on the first
    // frame with a device, so a capture can A/B any console dial without its own argv spelling.
    void setCamWobbleStop(i32 frame);
    void setConsoleSets(std::vector<std::pair<std::string, std::string>> sets);
    void setMeshHeapDefault(bool on);
    void setLodShareVertices(bool on);
    void setCamTranslate(f32 speedCmPerFrame);
    void setCamWander(f32 amp, f32 speed);        // --cam-wander AMP SPEED
    void setCamera(Vec3 pos, f32 pitchDeg, f32 yawDeg);
    void setScriptsDir(std::string d);            // --scripts <dir>
    void setSpawnTest(std::string cls);      // --spawn-test <ClassName>
    void setUnlitMode(bool on);                                  // --unlit
    // --view-mode lit|unlit|wireframe|rayhit-instance|rayhit-material|rayhit-distance|triangles|
    // undenoised: headless-verifiable twin of the viewport's view-mode popup. "undenoised" is an
    // independent toggle (sets undenoised_ only); every other name sets its Selectable's state,
    // clearing wireframe_/unlit_/debugView_ per the dropdown's mutual-exclusion rules. Unrecognised
    // name logs an error and changes nothing. Applied at startup once, like --unlit (main() calls this
    // once, before the first frame); logs one INFO line naming the mode applied.
    void setViewMode(const std::string& mode);
    void setPlayTest();                                       // --play-test
    void setPlayWalk() { defaultPawnWalk_ = true; }          // --play-walk: the no-GameMode default pawn walks
    void startDefaultPawnWalk(bool fromCamera);                // SandboxPlay.cpp: the capsule for a walking default pawn
    void driveDefaultPawnWalk(const Vec3& fwd, const Vec3& right);   // per frame, from the fly block
    void setProjectPath(std::string p);          // <path>.ocproject
    void setStartMode(std::string m);
    void setOpenMap(std::string p);
    void armBrowser(bool on);   // shows the start screen
    void setSingleInstanceEligible(bool b);

private:
    // applyProject adopts the project the browser or command line loaded and refreshes everything
    // keyed to it. LoadingScreen: opening a project is the longest blocking thing the editor does
    // after startup (reloads every material/mesh, cooks LOD pipelines, loads the start map, starts
    // the script host, all on the main thread between frames) -- the window stopped painting and
    // Windows greyed it out as "Not Responding". Same fix as the startup splash solved once already
    // ("naming the current stage turns 'it froze' into 'it is compiling shaders'"), reused one layer
    // up: scoped to the load, no early-return leak.
    struct LoadingScreen {
        Splash splash;
        bool on = false;
        Engine* borrowed = nullptr;   // non-null when reusing the engine's startup splash

        // `eng` is borrowed rather than owned when its startup splash is STILL UP (project opened
        // from the command line, inside onInit); opened from the browser instead, that splash is
        // long gone -- this distinguishes a second top-most window from the first.
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
        // Sets the stage text AND the completion fraction in one call -- the shape a staged level
        // load actually wants (SandboxLevelLoad.cpp's parse/environment/placements/foliage/finishing
        // bands, each a (text, fraction) pair as it starts). See progress() for what a borrowed
        // splash does with the fraction.
        void stage(const char* text, f32 fraction) {
            stage(text);
            progress(fraction);
        }
        // Sets the progress bar's fraction (0..1) under the status line, on whichever splash is up.
        //
        // NO EXPLICIT pump() HERE, unlike stage()'s: Splash::setProgress already repaints and pumps
        // (throttled to ~30 Hz) on its own -- see that method's own comment. A second, unconditional
        // pump() here would defeat the throttle on exactly the call site it exists for, since a level
        // load calls this at least every 256 placements.
        void progress(f32 fraction) {
            if (borrowed) { borrowed->setLoadingProgress(fraction); return; }
            if (!on) return;
            splash.setProgress(fraction);
        }
        ~LoadingScreen() {
            // No minimum visible time here (unlike startup, where a flashing splash reads as a
            // glitch) -- instant loads just don't show one. Borrowed splash belongs to Engine::run,
            // which closes it when startup finishes.
            if (on) splash.close(0);
        }
    };

    void applyProject(Engine& e);

    // Everything from here to drawRecoveryPrompt (maybeAutosave/maybeAutosavePrefs, checkForRecovery,
    // clearAutosave, drawRecoveryPrompt, selectionOutlineLines) is compiled UNGUARDED, unlike the old
    // landscape block this used to sit inside (see SandboxAutosave.cpp / SandboxViewport.cpp's "WHERE
    // THE TERRAIN BLOCK STARTS"): only this header was left behind guarded, so -DAVER_MODULE_LANDSCAPE
    // =OFF had definitions and callers for members that had ceased to be DECLARED, and must not lose
    // autosave/crash-recovery to a kill. Nothing here names a landscape type, function, or landscape_.
    void maybeAutosavePrefs(f32 dt);

    // ONE MESH PER MATERIAL: content_ splits a mesh naming several materials at load
    // (GameContent::buildMeshParts, partsFor) -- split rather than drawn as ranges because the ray
    // path's BLAS carries one material per instance. drawMeshParts is GONE: F4 (occlusion-fix-plan.md)
    // folded its job into planEntityDraws() (aver/game/SceneSubmission.hpp) plus game::drawWorld's
    // per-draw emitter.

    rhi::LineHandle selectionOutlineLines(Engine& e, u64 meshId);

    std::string autosavePathFor(const std::string& levelPath) const;

    void maybeAutosave(f32 dt);

    void autosaveCancelNotice();

    void autosaveRunSave();

    void clearAutosave();

    void checkForRecovery();

    void drawRecoveryPrompt(Engine& e);

    // Terrain block belongs here (same line SandboxViewport.cpp's copy of this split settled on):
    // everything below reads landscape_ or a landscape section's numbers, with no unguarded caller.
#if AVER_MODULE_LANDSCAPE
    bool createLandscapeForLevel(rhi::IDevice* device, u32 samples, f32 spacingCm);

    void recordLandscapeInLevel();

    // Points a live chunk generator at the resident section, so scattered entities sit ON the
    // terrain instead of on the flat plane at the chunk's origin Z.
    // CALLED FROM BOTH DIRECTIONS, because either can happen first: a level load can bring terrain in
    // while streaming is already running, and switching streaming on can find terrain already
    // resident. setChunkStreamingEnabled does the same wiring for the second case.
    // THE LAMBDA CAPTURES `this`, NOT THE DATA. Sculpting mutates the resident section (landscape_)
    // in place, so a copy

    void applyLandscapeToStreaming();

    void saveLandscape();
#endif

    void locateAverDesign() const;

    // Returns the directory the CLR host loads user assemblies from: --scripts, else the project's
    // Binaries\Scripts, else <exe>\Scripts.
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
    bool projectDirty_ = false;

    // Saved on edit, not on a button: a settings page needing a separate click to save will be
    // edited and closed with the edit lost (reported twice). The button stays as a confirmation, not
    // the only way through.
    // 0.5s debounce, twice the preferences debounce: this file is bigger and rewritten whole
    // (writeOcproject preserves comments/unknown keys, so every save re-serialises it); a drag
    // settles before it's written.
    static constexpr f32 kProjectAutosaveSec = 0.5f;
    f32 projectAutosaveAccum_ = 0.0f;


    // ---- FRAME BUDGET ----
    // Scales GI work to hit a target frame time. Off unless RENDER.FRAMEBUDGETMS is set -- a renderer
    // that silently changes its own quality can't be measured against, and this repo measures a lot.
    // Budgets the frame rather than gating on "camera is moving": a still camera in a heavy scene
    // misses the frame just as badly, and a moving camera in a trivial one doesn't need help.
    // Scales GI update interval first, then cone count -- MEASURED on Sponza under --cam-wobble:
    // raising the interval to 4 alone took GPU total 51.1ms -> 29.3ms (nearly the whole regression);
    // cone count 13->5 was worth ~8% of the primary pass. Render scale is NOT on this ladder even
    // though it's the biggest lever: rebuilding render targets mid-frame has taken the device down
    // before (see the renderScale recovery path); AverSR is the supported way to trade resolution,
    // chosen deliberately, not by a controller.
    // Hysteresis: dropping quality reacts quickly, raising it does not -- an oscillating controller
    // is more distracting than the frame it saves, since the eye tracks CHANGE in indirect light.
    // Controller: voxi::frameBudgetTick (FrameBudget.hpp), shared with the runtime; frameBudget_ is
    // its state.
    f32  frameBudgetMs_ = 0.0f;       // RENDER.FRAMEBUDGETMS; <= 0 disables the whole controller
    bool frameBudgetForced_ = false;  // --frame-budget: run the controller even in a capture
    // The two above stay UNGUARDED; these two do not. frameBudgetMs_ is a manifest key
    // (RENDER.FRAMEBUDGETMS) that the project apply/capture pair must round-trip whatever is compiled
    // in; neither it nor frameBudgetForced_ depends on a renderer existing. The state/tick below are
    // the controller itself (voxi::frameBudgetTick, FrameBudget.hpp) and name a render.voxi type
    // outright, so a VOXI=OFF tree had a member of a type it had never seen.
#if AVER_MODULE_VOXI
    voxi::FrameBudgetState frameBudget_;

    void frameBudgetTick(f32 dt, voxi::Settings& vs);
#endif

    void maybeAutosaveProject(f32 dt);
    bool projectRenderPending_ = false;   // manifest read before the device attached
    std::string projectSaveStatus_;

    bool hudPreviewActive() const;
    void setHudPreview(int index, f32 x, f32 y, f32 w, f32 h);
    int hudPreviewIndex_ = -1;
    int hudTest_ = -1;
    bool saveProject_ = false;
    // Derived Data Cache write-behind budget, in MB. Mirrors the renderer's byte value so the
    // slider has something to bind to; pushed across on load and on edit.
    f32 ddcRamBudgetMb_ = 256.0f;
    std::string saveLevelTo_;         // --save-level <out>
    bool saveLevelDone_ = false;
    bool rayProbe_ = false, rayProbeDone_ = false;   // --ray-probe <sx> <sy>
    f32  rayProbeX_ = 0.0f, rayProbeY_ = 0.0f;
    std::string importSrc_, importDst_;
    bool importDone_ = false;
    bool saveProjectDone_ = false;
    bool hudTestReported_ = false;
    f32 hudRectX_ = 0, hudRectY_ = 0, hudRectW_ = 0, hudRectH_ = 0;

    void saveLevelInteractive();

    // SAVE ALL: level plus every dirty asset tab (AssetEditorHost::saveAllDirty, which until
    // 2026-09-16 ran only from the quit prompt). File > Save All and Ctrl+Shift+S. Defined in
    // SandboxShell.cpp.
    void saveAll();

    // VIEWPORT SCREENSHOT to <project>/Saved/Screenshots (File > Take Screenshot, F9). The writer used
    // to exist only inside captureCheck's --frames gate. request...() sets the latch; service...()
    // runs once a frame beside captureCheck (SandboxRender.cpp), asks the device for a capture, and
    // writes the PNG cropped to the 3D viewport when it arrives.
    void requestViewportScreenshot();
    void serviceViewportScreenshot(Engine& e);
    u8  viewportShotState_ = 0;    // 0 idle, 1 requested, 2 capture requested and awaiting the image
    u64 viewportShotFrame_ = 0;    // engine frame the capture was requested on
    u32 viewportShotTries_ = 0;    // frames waited for the image; gives up rather than waiting forever

    // WINDOW TITLE kept in step with the open level and whether it has unsaved edits. Before 2026-09-16
    // it was set once when a project opened and never again. Defined in SandboxApp.cpp; called once a
    // frame and only touches the window when the text changes.
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

    bool autoCompile_ = false;
    // True when --auto-compile turned it on for this run: the stored preference neither overrides it
    // on load nor is overwritten by it on save (SandboxSettings.cpp).
    bool autoCompileFromCli_ = false;
    int focusLevelAt_ = 0;
    // Frames left before chunk streaming auto-enables; 0 = off. ON BY DEFAULT.
    // IT USED TO DEFAULT TO OFF, opening the editor on an empty world: without streaming nothing runs
    // the scatter generator, so a 33-species level showed its terrain and 14 hand-placed pines and
    // nothing else, with the fixing flag undiscoverable.
    // The delay is not cosmetic: frameCameraOnLevel moves the camera on level load, so enabling on frame 0
    // would stream a ring around the start position and immediately evict it.
    int chunkStreamAutoFrames_ = 5;
    int droneAutoFrames_ = 0;        // --drone: frames left before auto-enabling, 0 = off
    int undoTestAutoFrames_ = 0;     // --undo-test: frames left before runUndoTest() fires, 0 = off
    int keybindTestAutoFrames_ = 0;  // --keybind-test: frames left before it fires, 0 = off
    std::string keybindTestMode_;    // "write" or "read"
    static constexpr int kAutoCompileQuietMs = 500;
    std::chrono::steady_clock::time_point autoCompileDue_{};
    int autoCompilePending_ = 0;
    std::string autoCompileReason_;

    // Unloads the live script assemblies and loads the ones in binDir. Writes a status line.
#if AVER_MODULE_SCRIPTING
    bool reloadScripts(const std::string& binDir, std::string* status);
#endif

#if AVER_MODULE_FRAMEWORK
    void maybeSpawnTestActor();

    void capturePlayWorld();

    void restorePlayWorld();

    int32_t engineDefaultGameMode();
    int32_t engineDefaultGm_ = 0;
    // The view request as it stood before this editor overrode it -- see startPlay.
    bool    savedView_ = false;
    int32_t savedViewMode_ = AVER_FW_VIEW_THIRD_PERSON;
    float   savedViewEye_ = 160.0f, savedViewBoom_ = 450.0f;

    bool placePawnAtPlayerStart(const char* who);

    void startPlay();

    bool dronePlayActive() const;
    bool spectatorPlayActive() const;

    bool anyPlayActive() const;

    void stopPlay();

    void maybePieCameraTest();
    Vec3 pieCamMoveFrom_{0, 0, 0}, pieCamAfterMove_{0, 0, 0};
    f32  pieCamMoved_ = 0.0f, pieCamMoveDot_ = 0.0f, pieCamCoast_ = 0.0f;
    int pieCamFrames_ = 0, pieCamFrame_ = 0;
    Vec3 pieCamRef_{0, 0, 0};
    f32  pieCamRefYaw_ = 0.0f, pieCamRefPitch_ = 0.0f;
    f32  pieCamMaxPos_ = 0.0f, pieCamMaxYaw_ = 0.0f, pieCamMaxPitch_ = 0.0f;
    bool pieCamKeptYaw_ = false, pieCamKeptPitch_ = false;
    bool pieCamPendingLook_ = false;
    f32  pieCamWantYaw_ = 0.0f, pieCamWantPitch_ = 0.0f;

    void maybeInputSourceTest();
    int  inputSrcFrames_ = 0, inputSrcFrame_ = 0, inputSrcDisagree_ = 0, inputSrcStuck_ = 0;

    void maybeWheelSpeedTest();
    int  wheelTestFrames_ = 0, wheelTestFrame_ = 0;
    f32  wheelTestSpeedBefore_ = 0.0f;
    bool wheelTestForceFly_ = false, wheelTestSawImGui_ = false, wheelTestSawInput_ = false;
    bool wheelTestOwnAtWheel_ = false, wheelTestFlyAtWheel_ = false;
    bool inputSrcStateSaw_ = false, inputSrcImguiSaw_ = false;

    void maybeViewmodelTest();
    int  vmFrames_ = 0, vmFrame_ = 0;
    Vec3 vmRef_{0, 0, 0};
    f32  vmStill_ = 0.0f, vmMoving_ = 0.0f;
    u32  vmKids_ = 0;

    void maybeRecaptureTest();
    int recapFrames_ = 0, recapFrame_ = 0, recapLeaked_ = 0, recapNotRecaptured_ = 0;

    void maybeInputStuckTest();
    int  inputStuckFrames_ = 0, inputStuckFrame_ = 0, inputStuckLatched_ = 0, inputStuckFirstKey_ = -1;
    bool inputStuckSawDown_ = false;

    void maybePlayTest();

    void pushInput(bool uiActive);

    void drivePlayCamera();
#endif

    f32 viewAspect() const;
    bool inViewport(f32 mx, f32 my) const;
    Vec3 camForward() const;

#if AVER_MODULE_SCENE
    // ---- MULTI-SELECTION ----
    // Anchor plus set: the anchor is the existing sel_/selEntity_ pair, kept as-is since selection is
    // read in dozens of places (Details panel, gizmo, F-focus, outline, copy, rename, status line)
    // that each want ONE entity; multiSel_ is the rest.
    // INVARIANT (mirrors the Content Browser's cbSelection_): multiSel_ CONTAINS selEntity_ whenever
    // anything is selected, so an iterating caller sees the whole selection without adding the anchor
    // back -- forgetting that is how "delete removed all but one" bugs happen.
    std::vector<scene::Entity> multiSel_;
    // MEMBERSHIP for multiSel_, kept equal to it by every mutator (SandboxSelection.cpp, and the one
    // restore in SandboxMcp.cpp): multiIsSelected is asked per DRAWN entity per frame, and a linear
    // find over a big selection made that O(entities x selected) -- minutes of work a second on a
    // 51k-entity level after Select All. The vector stays the source of order (anchor, ranges).
    std::unordered_set<scene::Entity> multiSet_;
    void multiRebuildSet();                     // after any bulk rewrite of multiSel_

    bool multiStale() const;
    bool multiIsSelected(scene::Entity e) const;
    void multiSyncToAnchor();
    void multiClear();
    void multiSetSingle(scene::Entity e);
    void multiToggle(scene::Entity e);
    void multiRange(scene::Entity to);
    std::vector<scene::Entity> selectedEntities() const;
    // The rows the outliner lists, in the order a shift-click and Select All range over. A tree
    // rebuilds it every frame (drawOutlinerTreeRows): drawOutlinerRow records each parent root and
    // its OPEN rows as it walks them, and a run of childless roots is copied in whole; a FLAT list
    // (outlinerFlat_) has it built ONCE per cache rebuild instead. Both clip their childless rows
    // (ImGuiListClipper), so a per-row push_back could never see the rest. Read by multiRange,
    // selectAllInOutliner and the Select All enable checks (the Select menu, the viewport chord).
    std::vector<scene::Entity> outlinerOrder_;
    std::string outlinerFilter_;   // name filter box; empty = show everything

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
    using EditId = u32;

    // One undoable edit: a transform change, or a create/destroy of a scene entity or a placeholder
    // MeshObj (objects_). CreateObj/DestroyObj close a gap where the placeholder path pushed no undo
    // entry for an add/delete (deleteSelection()'s objects_ branch). asset/meshId/material, which this
    // struct used to carry directly, are gone: describeEntity() captures every component via
    // EntitySnapshot -- see EditorEntitySnapshot.hpp for what it omits (hierarchy; CName's internal
    // blob offsets).
    struct EditCmd {
        enum class Kind { Transform, Create, Destroy, CreateObj, DestroyObj, LandscapeStroke, FoliageStroke, Reparent, Material, Rename, RemoveComponent, Visibility, Collision, Animation };
        Kind kind = Kind::Transform;
        // Which edit this is, monotonically. Identifies the document's state so a save can record
        // "clean as of here" (levelHasUnsavedEdits). Never reused, so undo/redo cross a save point correctly.
        u64 serial = 0;
        EditId id = 0;            // a scene entity, through the indirection
        int objIndex = -1;        // or an objects_ index, for the placeholder scene
        EditXform before{}, after{};

        // Every other entity a multi-selection move took with it. Bug this closes: the gizmo's
        // multi-move applied the anchor's delta to every other entity with a bare setLocalTransform
        // and recorded nothing, so Ctrl+Z snapped only the anchor home and left the rest dragged,
        // with redo unable to repair it. One command, not N (unlike Delete's one-record-per-entity):
        // a drag is one gesture, so undoing it half-way would leave a state the author never saw.
        // Stored as LOCAL transforms, not world (before/after above are world-space and get converted
        // on the way back in), so undo can't drift through a conversion, and an entity whose parent
        // isn't in the selection restores to its actual local transform.
        struct AlsoMoved { EditId id = 0; Transform beforeLocal, afterLocal; };
        std::vector<AlsoMoved> alsoMoved;

        // VISIBILITY payload: every selected entity's AUTHORED-visible flag, before/after one click
        // on the Details panel's Visible checkbox -- same "one gesture, N entities" shape as
        // AlsoMoved above. `before`/`after` come from authoredVisible(); undo/redo replay them
        // through setAuthoredVisible(), which also drops the entity from editorHidden_, so undoing on
        // something H-hidden at the time makes it visible rather than reinstating a session-only hide.
        struct VisibilityChange { EditId id = 0; bool before = true; bool after = true; };
        std::vector<VisibilityChange> visibility;
        // COLLISION payload (Kind::Collision): the Details panel's Collides checkbox, before/after per
        // selected entity. Same {id, before, after} shape as VisibilityChange, so it reuses the type
        // (the bools mean "collides" here); replayed through setEntityCollide().
        std::vector<VisibilityChange> collide;
        // ANIMATION payload (Kind::Animation): the Details panel's Animation section, before/after per
        // selected entity (an empty clip = none); replayed through applyEntityAnim().
        struct AnimChange { EditId id = 0; EntityAnim before, after; };
        std::vector<AnimChange> animation;
        std::string label;        // outliner display name; editor-owned bookkeeping, not World's
#if AVER_MODULE_SCENE
        editor::EntitySnapshot snap;    // scene entity: asset name, persisted id, every other component
#endif
        // Whether to give the recreated/pasted/duplicated entity a body, not what SHAPE it is --
        // rebuildEntityBody derives the shape fresh each time, so nothing carried can go stale.
        bool hadBody = false;
        // The two authoring flags that are NOT components (not in `snap`): nocollide/snapToGround
        // are load-time instructions from the .ocworld PLACE record, held only in
        // entityCollide_/entitySnapZ_ since there is no component for captureEntity to find. Bug this
        // closes: destroyEntity erased both maps and nothing put them back, so Ctrl+Z after a delete
        // restored the entity minus its nocollide -- silently solid after the next save+load.
        bool  hadCollide = true;      // the default saveLevel writes for an entity it has no entry for
        bool  hadSnapZ   = false;
        f32   snapZ      = 0.0f;
        // Its object animation (empty clip = none). Not in `snap`: captureAuthored leaves the CAnimator
        // out, since Play advances its clock and entityAnim_ holds the authored values.
        EntityAnim hadAnim;
        // Its `vehicle` preset (empty = not a car), held by level_ and so not in `snap` either. Restored
        // by recreateFrom only: Copy and Duplicate make ordinary meshes.
        std::string hadVehicle;
        MeshObj objSnapshot{};    // CreateObj/DestroyObj payload; MeshObj is trivially copyable
#if AVER_MODULE_PBR
        // Material payload: THE WHOLE DESC, both sides, not just the slider that moved -- a
        // MaterialDesc is a few hundred bytes and its controls interact (ior/reflectance, alpha
        // mode/transmission), so replaying one field could restore a state that never existed. Undo
        // depth is 64, which bounds the cost.
        pbr::MaterialHandle matHandle = 0;
        pbr::MaterialDesc matBefore{}, matAfter{};
#endif
        // Rename payload (`label` above already carries the NEW name for a Create). These are the
        // outliner label, not CName -- see applyEntityLabel for why the display name is what's edited.
        std::string renameBefore, renameAfter;

#if AVER_MODULE_SCENE
        // RemoveComponent payload: removed component's type and byte-exact contents, captured just
        // before removal. Reuses EntitySnapshot::Comp, the same shape captureEntity's loop uses for `snap`.
        editor::EntitySnapshot::Comp removedComponent;
#endif

        // LandscapeStroke payload: the heightfield sub-rectangle a brush stroke touched, before/after.
        // A RECT DIFF, not a section snapshot: a 512x512 section is a megabyte of floats (a hundred MB
        // on the undo stack per minute of painting at full copies); a touched rect is usually KB.
        // One entry per stroke, not per frame: rect is UNIONED across the stroke, pushed once on
        // release, or a two-second drag would take sixty Ctrl+Z presses to undo.
        u32 landX0 = 0, landY0 = 0, landX1 = 0, landY1 = 0;   // inclusive sample bounds
        std::vector<f32> landBefore, landAfter;

        // FoliageStroke payload: every entity one brush stroke created or erased, as one entry --
        // same reason as LandscapeStroke: a brush places dozens of instances/second, so a Create per
        // instance would cost a hundred Ctrl+Z per drag. recreateFrom() undoes an erase from these snapshots.
#if AVER_MODULE_SCENE
        std::vector<editor::EntitySnapshot> batchSnaps;
        std::vector<EditId>                 batchIds;
#endif
        bool batchWasErase = false;   // which direction undo has to run

#if AVER_MODULE_SCENE
        // Destroy's DESCENDANTS, parents before children, excluding the root (`id`/`snap` above).
        // Bug this closes: World::destroy retires the whole subtree, so a Destroy carrying one
        // snapshot could only restore one entity -- delete a table with a lamp parented to it, Ctrl+Z,
        // and the table returns with the lamp gone for good. That is data loss, not a missing nicety.
        // `parent` indexes THIS vector (-1 = the command's own root), not an EditId, since the whole
        // subtree is rebound in one go and an id would have to be re-resolved mid-restore.
        struct DestroyedNode {
            EditId id = 0;
            i32 parent = -1;
            EditXform xf{};
            std::string label;
            editor::EntitySnapshot snap;
            bool hadBody = false;   // see EditCmd::hadBody's own comment
            // See EditCmd's own copy above: not components, so not in snap.
            bool  hadCollide = true;
            bool  hadSnapZ   = false;
            f32   snapZ      = 0.0f;
            EntityAnim hadAnim;
        };
        std::vector<DestroyedNode> subtree;

        // The destroyed entity's own parent, as an EditId (0 = was a root). `subtree` only restores
        // what's BELOW the named entity; without this, undoing a child delete put it back at the top
        // level, and, because its transform is stored parent-relative, offset from the world origin
        // instead of its actual parent.
        EditId parentId = 0;

        // Reparent's before/after parent, as EditIds (0 = root). Separate fields from parentId above,
        // even though the shape matches: that field is Destroy-specific, and one field carrying two
        // meanings across two Kinds is drift a later reader can't detect. The before/after EditXform
        // pair rides the existing `before`/`after` members.
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
        for (const scene::Entity ent : multiSel_) {
            if (ent == selEntity_ || !w.valid(ent)) continue;
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

    bool    foliageStroking_ = false;
    f32     foliageAccum_    = 0.0f;
    EditCmd foliageBatch_{};
    // One entry per instance THIS SESSION'S BRUSH placed: position (matched by erase) and the
    // collision radius it was placed with (0 for collisionRadiusCm == 0 types -- excluded from the
    // interpenetration test on both sides, like ChunkGenerator.cpp's placedSolid).
    struct FoliagePlaced { Vec3 pos; f32 solidRadiusCm; };
    std::vector<FoliagePlaced> foliagePlaced_;
#endif
#endif

    void undo();

    void redo();

    std::string selectionLabel() const;

    f32 selectedRadius() const;

    // ---- FOUR GROUPS THAT SAT INSIDE `#if AVER_MODULE_SCENE` AND ARE READ FROM OUTSIDE IT ----
    // Plain arithmetic/flags, no scene:: type, each read unconditionally -- landed inside the guard by
    // accident; module-matrix's `scene-off` row caught the resulting compile failure, a line hundreds
    // away from the guard.
    //
    // Preference autosave timer: maybeAutosavePrefs (SandboxAutosave.cpp) runs unguarded, persisting
    // editor-wide settings (fly speed, wireframe, tile size), not a level/world. 0.25s, not the level
    // autosave's longer interval: prefs are cheap to check/write, so a miss is cheap too (was 2s,
    // too long -- an unsaved change looked identical to a never-saved one). Not zero: a dragged
    // slider dirties every frame, and 0.25s collapses a one-second drag to 4 writes instead of 60.
    // No I/O when nothing changed (setPrefString/flushEditorPrefs early-out on no dirty state).
    static constexpr f32 kPrefsAutosaveSec = 0.25f;
    f32 prefsAutosaveAccum_ = 0.0f;

    // THE TWO DEFERRED AUTOSAVE ANSWERS: drawNotifications writes them, unguarded maybeAutosave
    // consumes them next tick (a scene-less build still ticks the autosave machinery). Deferred, not
    // acted on inline, because the buttons draw from onRender, which runs AFTER onUpdate -- acting
    // immediately would apply a postpone to a save that had already happened this frame.
    bool autosavePostponeRequested_ = false;
    bool autosaveRetryRequested_ = false;

    // Create-a-landscape knobs: grid resolution + tile spacing, read/written by the landscape mode
    // panel (guarded on AVER_MODULE_LANDSCAPE alone; LANDSCAPE implies neither direction of SCENE).
    int landCreateSamples_ = 513;
    f32 landCreateSpacingCm_ = 100.0f;

    // --no-editor-chrome: suppress everything the editor draws ON TOP of the scene, so a capture can
    // be compared against AverEngineRuntime.exe's. Run-scoped, never persisted (see argv loop's flag comment).
    bool noEditorChrome_ = false;

    // ---- PlayerStart: where the player spawns in ----
    // Level format already had the answer, unread: OcWorldData's hasSpawn/spawnX/Y/Z/Yaw were
    // parsed/written but consulted nowhere -- not begin_play, not loadLevel, not saveLevel. ONE
    // RECORD, SO ONE MARKER: SPAWN is scalar, not a list, so PlayerStart must not become a second,
    // independent thing that can disagree with it (two sources of truth, one invisible) -- same
    // live-handle shape as hasLevelSun_/hasLevelFog_. TRANSIENT ENTITY, never pushed to
    // levelEntities_ (like the drone): must not also save as PLACE. cbDeleteRefs_/cbRenameRefs_ sit
    // OUTSIDE `#if AVER_MODULE_SCENE` though written inside it: cbFindReferencesTo, their only
    // producer, is an unconditional text scan over .ocworld/.ocmap/.ocmat/.ocgraph/.ocproject files,
    // reading no world. Filled when the delete-confirm modal opens; cleared on delete/cancel so a
    // later modal never shows a stale answer.
    std::vector<std::string> cbDeleteRefs_;
    std::vector<std::string> cbRenameRefs_;   // the same, for the rename dialog
    // Whether the rename dialog repoints what it found. ON by default: repointing is what an author
    // wants nearly every time; the checkbox lets a tool editing other people's files opt out.
    bool cbRenameRepoint_ = true;

#if AVER_MODULE_SCENE
    scene::Entity playerStart_ = scene::kInvalidEntity;

    // The Outliner row currently being renamed in place, and its edit buffer.
    // Create-a-landscape controls; the shape knobs are landscape_.noiseParams(), shared with the
    // ring generator so a created section and the tiles around it come from one set of numbers.
    // Autosave interval, TEN MINUTES (was 30s, until the countdown made that cadence visible). The
    // old value assumed autosave was silent ("a crash costs a gesture or two"); what changed is that
    // autosaveRunSave does NOT call markLevelSaved (a sidecar isn't a real save), so the timer
    // restarts immediately and a 10s warning on a 30s period put a countdown on screen a third of
    // every minute -- a safety net nobody can ignore is one they turn off. Ten minutes matches
    // Unreal's interval, for the same reason; the 10s warning window is now 1.7% of the period
    // instead of 33%.
    static constexpr f32 kAutosaveDefaultSec = 600.0f;
    f32 autosaveIntervalSec_ = kAutosaveDefaultSec;
    f32 autosaveAccum_ = 0.0f;

    // Why three states, not a bool: the save is synchronous (saveLevel writes from inside onUpdate,
    // which runs BEFORE onRender), so a "Saving..." notification raised and saved in the same tick
    // would be replaced by "Saved" before buildUI ever draws it. Pending spends one presented frame
    // saying so, then writes on the tick after.
    enum class AutosaveState : u8 { Idle, Counting, Pending };
    AutosaveState autosaveState_ = AutosaveState::Idle;
    u64 autosaveNotify_ = 0;      // the countdown's notification, updated in place
    int autosaveShownSec_ = -1;   // last whole second rendered, so updates are 1/s not 1/frame
    u8  autosavePostpones_ = 0;
    // Ten seconds is long enough to finish a drag and press Postpone, short enough that the warning
    // is about the save that is coming rather than a background fact.
    static constexpr f32 kAutosaveWarnSec = 10.0f;
    // A Postpone that works forever is a way to switch the safety net off without ever deciding to.
    static constexpr u8  kAutosaveMaxPostpones = 3;
    bool autosaveWritten_ = false;
    bool autosaveFailedWarned_ = false;
    std::string recoveryPath_;      // a sidecar newer than its level, waiting to be offered



    // --scene-census + the latch that fires it exactly once. Emitted from onUpdate, not the load:
    // class placements spawn LATER than loadLevel, and a census at load would miss them -- the exact
    // divergence this exists to catch.
    bool sceneCensus_ = false;
    bool sceneCensusDone_ = false;

    scene::Entity outlinerRenaming_ = scene::kInvalidEntity;
    // The row whose right-click menu was open last frame. The flat Outliner only submits the rows
    // its clipper hands out, and a popup lives inside its row's BeginPopupContextItem, so the clipper
    // pins this row (like the renaming and dragged ones) or scrolling it away would strand the menu.
    scene::Entity outlinerCtxRow_ = scene::kInvalidEntity;
    bool outlinerRenameFocus_ = false;
    char outlinerRenameBuf_[128] = {0};

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

    void spawnPrimitive(Engine& engine, const char* assetPath, const char* label);

    void spawnCube(Engine& engine);

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

#if AVER_MODULE_SCENE
    // ---- VIEWPORT PLACEMENT VERBS, dispatched from handleManip's edit-verb block and defined in
    // SandboxViewport.cpp. Each acts on the whole multi-selection as ONE undo entry. End: drop each
    // selected entity onto whatever is below it (rayPickGeometry + dropRestLift).
    // snapSelectionToFloor is guarded on AVER_WITH_IMGUI too, matching its definition: its one
    // caller is editor::CommandId::SnapToFloor in the viewport's chord handler, so both the scene
    // and the editor UI gate it -- a declaration visible where the definition is compiled out is
    // one call away from an unresolved external. Latent not live: the caller was already guarded
    // on both.
#if AVER_WITH_IMGUI
    void snapSelectionToFloor();
#endif
    // Arrow keys / PageUp / PageDown: move by the move-snap step (or 10 cm with snapping off).
    void nudgeSelection(const Vec3& deltaCm);

    // AUTHORED visibility: what the Details panel's Visible checkbox shows and saveLevel writes
    // (OcWorldPlacement::visible), distinct from H/Shift+H/Ctrl+H's SESSION-ONLY hide below. True
    // when kMeshRendererVisible is set OR the entity is in editorHidden_, so an H-hidden entity
    // still saves/reopens visible.
    bool authoredVisible(scene::Entity e) const;
    // Sets the bit directly and drops `e` from editorHidden_: an authored edit supersedes whatever
    // temporary H-hide state the entity was in, so the bit alone is the truth again afterward.
    void setAuthoredVisible(scene::Entity e, bool v);

    // H hides the selection, Shift+H hides everything else, Ctrl+H restores what these hid.
    // SESSION-ONLY like every H verb: not because a level can't store visibility (it can; see
    // authoredVisible above) but because H is deliberately temporary, like Unreal's own H.
    void hideSelection();
    void isolateSelection();
    void unhideAll();
    std::vector<scene::Entity> editorHidden_;   // what hideSelection/isolateSelection turned off
    // The union of the whole selection's world bounds, for F (Frame Selected). selectedXform and
    // selectedRadius describe the ANCHOR, which the gizmo needs; framing needs the set.
    bool selectionBounds(Vec3& center, f32& radius) const;
    // MARQUEE SELECT: a left-drag that starts on empty viewport space draws a rectangle; on release
    // every eligible entity whose projected bounds intersect it is selected (Ctrl adds to the set).
    bool nudgeEditOpen_ = false;   // a held nudge run's one undo entry is open (see handleManip)
    bool marqueeArmed_ = false;    // mouse went down on empty space; becomes a marquee past a small drag
    bool marqueeActive_ = false;
    f32 marqueeX0_ = 0.0f, marqueeY0_ = 0.0f, marqueeX1_ = 0.0f, marqueeY1_ = 0.0f;
#endif

#if AVER_MODULE_LANDSCAPE && AVER_WITH_IMGUI
    void handleSculpt(Engine& e, const ImGuiIO& io, bool overScene, f32 mx, f32 my);
#endif

    void drawGizmo(Engine& e);

#if AVER_MODULE_LANDSCAPE
    void drawSculptCursor(Engine& e);
#endif

    // The --autosave-test latches: plain bools, moved out of AVER_WITH_IMGUI (which proves nothing
    // about the scene) since maybeAutosave (SandboxAutosave.cpp) reads autosaveTestLift_/
    // autosaveTestWarned_ from a body guarded on AVER_MODULE_SCENE alone.
    bool autosaveTestArm_ = false;   // --autosave-test: mark the level dirty once, then let it run
    bool autosaveTestLift_ = false;  // ...and lift the capture guard, loudly (see maybeAutosave)
    bool autosaveTestWarned_ = false;

#if AVER_WITH_IMGUI
    void deleteSelection();

    void copySelection();

    void pasteClipboard();

    void duplicateSelection();

    void runCbMoveTest(const std::string& root);
    std::string cbMoveTestDir_;   // --cbmove-test <dir>
    int cbMoveTestFrames_ = 0;

    void runNotifyTest();

    void runPrefsWriteTest();
    int prefsWriteTestFrames_ = 0;
    int  notifyTestFrames_ = 0;      // --notify-test: frames left before the samples are raised
    bool notifyTestLift_ = false;    // ...and the one thing that lets them draw in a bounded run

    // --project-switch-test: opening a project with NO render settings must not leave the previous
    // project's settings in force. Synthetic: drives applyProjectRenderSettings with two hand-built
    // ProjectDescs, not real .ocproject files, since what's under test is that function's guard, not
    // the parser -- valid() needs only a name+dir, so this runs headless anywhere.
    // Falsifies a specific regression: projectBackend_/frameBudgetMs_ used to be assigned BELOW the
    // hasRenderSettings() early return, so a bare project inherited both, then wrote the inherited
    // backend into its own manifest on the next unrelated edit.
    int projectSwitchFrames_ = 0;
    void runProjectSwitchTest();

    // --validate-graph <path>: runs the managed graph validator over one .ocgraph and prints the
    // answer. END-TO-END ON PURPOSE: C++ -> ScriptHost -> hostfxr -> HostBridge.GraphValidate ->
    // OcGraphParser/Graph.Validate and back with a real message -- a stub (GraphEditorLoadSaveTest)
    // can't prove the export is reachable, the buffer contract holds, or the .NET runtime is
    // hosted. No "exports no GraphValidate" warning at startup only proves a symbol bound; this
    // proves it RUNS.
    // --graph-print-test: drives the on-screen graph-print feed's SINK, where its real logic lives.
    // Not reachable from a headless suite: logSink is a static member installed into the core logger,
    // depending on this instance's deque. Logs through AVER_INFO rather than poking graphPrints_
    // directly, so the "[Graph] " prefix match is under test too -- a mismatch with GraphInterop
    // would write to the log and never reach the overlay, silently.
    int graphPrintTestFrames_ = 0;
    void runGraphPrintTest();

    // --clear-shader-cache: empty the DXIL blob cache and report what went. Second half of the
    // unbounded-cache problem: a size bound (D3D12Device::init) stops unlimited growth, but gave no
    // way to clear deliberately -- needed when a DXC upgrade changes DXIL for identical input
    // invisibly to a content-keyed cache, or disk space is short. A flag, not a menu item, for now:
    // a maintenance action, runnable without opening the editor.
    int clearShaderCacheFrames_ = 0;
    // Which cache to clear. Empty = the real one under the user's data directory. An explicit path
    // is accepted so the command is checkable end to end without deleting real blobs (running it
    // against the dev's own 59MB cache would cost an unwanted recompile) -- also what CI/a build
    // server wants.
    std::string clearShaderCacheDir_;
    void runClearShaderCache();

    std::string validateGraphPath_;
    int validateGraphFrames_ = 0;
    void runValidateGraph();

    // --rename-repoint-test: the rename-repoint chain over REAL files in the real project.
    // AssetRefScanTest covers the string surgery exhaustively but not this: which files open, that
    // scan and rewrite agree on the same set, that the write lands on disk, and that a near-miss
    // file beside a real referrer stays byte-identical -- the parts that edit somebody's project.
    // Writes into a scratch folder UNDER the project's content root (the only place
    // cbFindReferencesTo looks), removed afterwards.
    int renameRepointFrames_ = 0;
    void runRenameRepointTest();

    std::string findRefsPath_;
    int findRefsFrames_ = 0;
    void runFindRefs();

    void runSaveDirtyTest();
    int saveDirtyTestFrames_ = 0;

    void runMultiSelectTest(Engine& eng);
    int multiSelTestFrames_ = 0;   // --multiselect-test: frames left before it fires

    // --asset-assign-test: the asset picker's three assignment helpers, headlessly. The picker itself
    // is an ImGui popup and can't be driven here, but nothing interesting lives in it -- what can go
    // wrong is the assignment: which id space each field uses, re-upload, dirty marking. Extracted to
    // plain C++ for exactly this. THE MATERIAL CASE IS THE POINT: a material is an INTERNED NAME
    // TOKEN while mesh/effect are fnv1a64 path hashes, so a path hash written into mr->material fails
    // SILENTLY -- resolves to no surface or an unrelated one, and the entity just renders wrong.
    // --graph-hits-test <graph.ocgraph>: the node-hit chain end to end, through the REAL bridge.
    // The managed half has its own tests (NodeHitTests.cs: branch arms, diamonds, graph bleed),
    // which can't prove anything on this side of the ABI -- that the two new exports bind, a graph
    // name marshals, the "nodeId:age;..." payload round-trips, and disarming stops it; a stub can't
    // fail those. Driven through graphLoad/graphTick (one graph, one entity, no class registry/Play
    // state) -- the smallest thing that runs real compiled IL.
    std::string graphHitsTestPath_;
    int graphHitsTestFrames_ = 0;
    void runGraphHitsTest();

    int assetAssignTestFrames_ = 0;
    void runAssetAssignTest();

    void runUndoTest(Engine& eng);

    void runKeybindPersistTest(const std::string& mode);

    void viewportRay(f32 screenX, f32 screenY, Vec3& ro, Vec3& rd) const;

#if AVER_MODULE_SCENE
    const aver::editor::PickGeometry& pickGeometryFor(u64 meshId);
#endif

    // Returns true when the click landed on something (an entity or a placeholder object),
    // including the Ctrl-toggle path; false on a Ctrl-click MISS, which leaves
    // sel_/selEntity_/the multi-selection untouched. See its own definition for why.
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

    // The serial the level was last written at. 0 means "as loaded, unedited".
    u64 savedEditMark_ = 0;
    u64 editSerialNext_ = 0;

    void requestExitChecked(Engine& e);

    void drawAboutPrompt(Engine& e);

    void drawSaveLevelAsPrompt();

    void drawExitPrompt(Engine& e);

    void openLevelPickerNow();

    void drawOpenLevelPrompt(Engine& e);

    void drawPendingOpenPrompt(Engine& e);

    // File > Launch in Aver Engine Runtime: starts AverEngineRuntime.exe on the open level, asking
    // first if the level has unsaved edits (drawLaunchRuntimePrompt) the same way an open would.
    // `skipDirtyCheck` is how the prompt's own buttons launch afterwards without re-asking.
    void launchInRuntime(Engine& e, bool skipDirtyCheck = false);

    void drawLaunchRuntimePrompt(Engine& e);

    void drawUpgradePrompt();

    // Outside `#if AVER_WITH_IMGUI` (unlike loadEditorPreferences/buildEditorPrefs beside which it
    // was written): its body (SandboxSettings.cpp) only calls setPrefBool/setPrefFloat/setPrefInt, no ImGui::, while both
    // callers (onShutdown, maybeAutosavePrefs) compile in every configuration -- AVER_ENABLE_UI=OFF
    // dropped the declaration but not those calls (the `no-ui` matrix row's failure).
    void saveEditorPreferences();

    void buildUI(Engine& e);

#if AVER_WITH_IMGUI
    // ---- Revision control: the editor-facing half of RevisionControl.hpp ----
    // NOTHING HERE SPAWNS A PROCESS: revisionControlRefresh()/revisionControlSelect() hand the
    // question to a worker and return; the rest only read what a finished worker latched (see the
    // RcStatusQuery block among the members for why that's structural, not polite).
    // revisionControlTick reaps whatever finished and starts a refresh when the panel or Content
    // Browser is on screen and the latched answer has gone stale. Called once a frame from buildUI.
    void revisionControlTick();
    // `force` is the Refresh button: it asks again even when the last answer was "no git here",
    // which the timer deliberately does not retry.
    void revisionControlRefresh(bool force);
    // Starts the history + diff query for one repo-relative path (git's own spelling, straight out
    // of editor::FileEntry::path -- never a path this editor assembled).
    void revisionControlSelect(const std::string& repoRelativePath);
    void buildRevisionControlPanel();

    // ---- the bottom status bar's right-hand widgets ----
    // ONE HELPER, NOT TWO HAND-ROLLED BUTTONS: an icon and a word, tinted by state, that answers a
    // question on hover and opens a menu on click -- both status widgets are exactly this, and the
    // bar's three drawer buttons already share a lambda for the same reason.
    // RETURNS WHETHER IT WAS CLICKED; the popup itself belongs to the caller, the only differing part.
    bool statusBarWidget(const char* id, const char* face, const ImVec4& tint, const char* tooltip);

    // Reads the SAME latched answer the panel reads, through editor::summariseForStatusBar, and
    // chooses only a colour for it. See that function's own comment for why the states are decided
    // in RevisionControl.hpp rather than here.
    void drawRevisionControlStatusWidget();
    static ImVec4 rcMoodColour(editor::RepoMood m);

#if AVER_MODULE_MCP
    // The control-channel widget: whether the loopback listener is up, and the one place it can be
    // started or stopped by hand. Guarded on the module, because a button offering to start a
    // channel that was never compiled in is a button that lies.
    void drawMcpStatusWidget();
#endif
    // An absolute editor path as git would name it: repo-relative, forward slashes, empty when the
    // path is outside the repository (or when no repository is known yet).
    std::string rcKeyFor(const std::string& absolute) const;
    // What the Content Browser should mark this entry with. False means git had nothing to say
    // about it, which for a tracked file means it matches HEAD and the index.
    bool rcMarkFor(const std::string& absolute, bool isDir, editor::FileStatus& out) const;
    // The badge colour, shared by the gallery, the list and the panel's own file table so one
    // status cannot read as two different colours in two views of the same repository.
    static ImU32 rcStatusColour(editor::FileStatus s);

    void drawGraphPrintOverlay(ImVec2 vpMin, ImVec2 vpMax);

    static void logLineStyle(LogLevel l, ImVec4& text, ImVec4& row, bool& filled);

    static void drawLogLine(LogLevel level, const char* text);

    void drawOutputLog();

    static std::string elide(const std::string& s, std::size_t maxLen);

    // One row of the live suggestion popup: what to SHOW (name plus a short description) and what to
    // INSERT if it is picked (the bare name, so the same row serves a command or a variable).
    struct ConsoleSuggestion { std::string display; std::string insert; std::string tooltip; };

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

    // One root the Content Browser mounts.
    struct CbRoot { const char* label; std::string path; bool engine; };
    std::vector<CbRoot> cbRoots() const;

    void cbNavigate(const std::string& dir);

    bool cbCanBack()    const;
    bool cbCanForward() const;
    // Every selected path in the current folder. See cbClickSelect for why cbSelectedFile_ stays
    // alongside it rather than being replaced by it.
    std::vector<std::string> cbSelection_;
    // The paths the current folder is showing, refreshed every frame the browser draws. Ctrl+A's
    // source; see drawFolderFiles.
    std::vector<std::string> cbShownPaths_;

    void cbBack();
    void cbForward();

    std::string cbParentDir() const;

    static bool cbIsSourceFile(const std::string& ext);

    const editor::IdeInfo& cbIde() const;

    void cbOpenEntry(const std::string& full, bool isDir);

    void cbInvalidate(const std::string& dir);

    bool cbIsEditable(const std::string& path) const;

    // A drop onto a folder, latched at drop time and answered by the Copy/Move prompt a frame
    // later. See cbFolderDropTarget for why it cannot be answered where it happens.
    std::vector<std::string> cbMoveSources_;
    std::string              cbMoveDest_;
    bool                     cbWantMoveOrCopy_ = false;

    // ---- creating a new asset in the browser ----
    // Three formats created here share everything except the bytes written, so the common half is
    // these two helpers rather than a fourth copy of the loop; each "New X" item is: pick a free
    // path, write, adopt.
    // NO NAME PROMPT, unlike New Folder: lands as New<Kind>.<ext>, renamed via the browser's existing Rename.

    std::filesystem::path cbFreeAssetPath(const char* stem, const char* ext);

    void cbAdoptNewAsset(const std::filesystem::path& target, bool openEditor = true);

    // Guarded like cbCreateParticleEffect: builds a starter pbr::MaterialDesc for fmt::newMaterialScript,
    // and with the material system compiled out there's neither a type to build nor a reader for the
    // file. The Content Browser's "New Material" item carries the same guard.
#if AVER_MODULE_PBR
    void cbCreateMaterial();
#endif

    void cbCreateSoundGraph();

    // Writes a starter .ocparticle (a small warm ember burst) and opens it -- a format with a
    // working editor tab and icon that, until now, nothing could create from inside the editor.
    // Guarded: editor::pxStarterEffect (ParticleEditor.hpp) and fmt::OcParticleExtras/saveOcparticle
    // (OcParticle.hpp) are unreachable with AVER_MODULE_PARTICLES off -- see the #include guard
    // near the top of this file.
#if AVER_MODULE_PARTICLES
    void cbCreateParticleEffect();
#endif

    void cbCreateFoliageType();

    void cbCreateNodeGraph();

    void cbCreateBehaviourTree();

    // Writes a starter .ocinput (Input Scheme) and opens it -- same shape as cbCreateFoliageType
    // above: a format with a working editor tab nothing could create from inside the editor before.
    void cbCreateInputScheme();

    std::string cbImportBlockedReason(const std::string& dir) const;

    std::string cbMoveDragPayloadFor(const std::string& dragged) const;

    static bool cbIsUnder(const std::string& path, const std::string& dir);

    void cbFolderDropTarget(const std::string& folderPath);

    std::vector<std::string> cbPruneNested(const std::vector<std::string>& in) const;

    bool cbCopyEntryTo(const std::string& src, const std::string& destDir);

    bool cbMoveEntryTo(const std::string& src, const std::string& destDir);

    std::vector<std::string> cbFindReferencesTo(const std::string& absPath) const;

    // ---- REPOINTING WHAT REFERENCED AN ASSET, after it has been renamed ----
    // Rewrites `oldRel` to `newRel` in every text asset that anchored-matches it, in place. Returns
    // {files changed, references rewritten, files it could not write}.
    // WHAT THIS CANNOT DO (stated rather than silently missed):
    //  - A reference stored as a HASH with no path text (.ocmat TEX `{guid:0x...}`, save-game
    //    fnv1a64 ids) -- nothing textual can find or fix those.
    //  - Anything outside the five text formats the scan reads (.ocmesh material slots, .ocbt's
    //    string table, a C# file building a path in code).
    //  - A material named by FILENAME STEM rather than path (materialForSurface probes three dirs).
    // NOT TRANSACTIONAL: N separate file writes, each atomic (writeFileTextAtomic) so no individual
    // file is left torn, but a failure partway leaves some updated and some not -- the failure count
    // is returned so the caller can say so.
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

    void drawFolderTree(const std::string& dir);

    // Returns an ASSET sheet tile for an engine asset extension, or -1 -- what an asset looks like
    // with no art for it. Separate from the source-file sheet (different textures/provenance).
    // Only four extensions had sprite-sheet tiles; everything else fell through to one identical grey
    // page, so a folder of twenty asset types read as twenty identical documents. GLYPH PLUS
    // COLOUR, not colour alone: colour separates types at a glance, glyph says WHICH
    // up close (Material Icons already in the UI font, so it scales as text). Real thumbnails needed
    // RHI texture-to-texture copy/readback (copyTexture) to survive frame K+1 reusing the target --
    // ThumbnailCache is the first consumer; this glyph is the fallback until one exists.
    struct AssetKind { const char* icon; ImU32 tint; const char* label; };
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


    // True when hay contains needle, ignoring case. An empty needle matches.
    // ---- the asset picker ----
    // Why there was none: assigning an asset meant dragging it out of the Content Browser (closing
    // that drawer made reassignment impossible), and only CParticleEmitter::effect even had a drop
    // target -- CMeshRenderer::mesh printed a hex id and offered nothing, CMeshRenderer::material
    // offered nothing either, and isPlaceableAssetExt only lets
    // .ocmesh/.ocparticle start a drag at all. A button beside the field bypasses all of that.
    // ONE GENERIC WIDGET over (label, id) candidates, not a picker per field: the three fields differ
    // only in where candidates come from and what an id MEANS. Shaped after the graph editor's node
    // palette (search-on-open, case-insensitive filter, a capped list that says so).
    // Candidates come from maps a project reload clears (meshPathById_, surfaceMaterials_), so they
    // are never kept beyond the popup that asked for them: the caller builds them only while the
    // popup is OPEN (assetPicker draws nothing otherwise), and a PickerCands below holds them across
    // the consecutive frames the popup stays open -- a closed popup costs nothing, and an open one
    // does not re-read the project's Materials folders or re-sort every mesh path each frame.
    struct AssetChoice { std::string label; u64 id = 0; };

    bool assetPicker(const char* popupId, const std::vector<AssetChoice>& candidates,
                     u64 current, u64* picked);
    char assetPickerFilter_[64] = {};

    // One picker's candidate list, valid only while it was last used on the PREVIOUS frame or this
    // one (`lastFrame`, ImGui's frame count -- a gap means the Details panel was not drawn in
    // between, and the list is rebuilt) and while `stamp` (the size of the map it was built from)
    // still matches. A frame that sees the popup CLOSED calls drop(), which frees the list.
    struct PickerCands {
        std::vector<AssetChoice> list;
        int   lastFrame = -2;
        usize stamp = 0;
        void drop() { std::vector<AssetChoice>().swap(list); lastFrame = -2; }
    };
    PickerCands pickMeshCands_, pickMaterialCands_, pickEffectCands_;

    // ---- the three assignments, each its own function since an "asset id" is three things ----
    // NOT UNDOABLE, a TESTED CONTRACT not an oversight: runSaveDirtyTest asserts a Details-panel
    // asset write marks the level dirty via markLevelUnsaved() with no EditCmd. Changing that is a
    // deliberate call, not the picker's to make.
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

    // overwrite: the Import dialog's explicit choice to replace what an import would collide with.
    void importAsset(const std::string& src, const std::string& destDir, bool overwrite = false);
    // FILES DROPPED ON THE WINDOW FROM EXPLORER (Window::takePendingDroppedFiles, polled in onUpdate).
    // Imports each into the Content Browser's current folder through importAsset. Defined in
    // SandboxContentBrowser.cpp.
    void importDroppedFiles(const std::vector<std::string>& paths);

#if AVER_HAVE_AUDIO_IMPORT
    void importAudio(const std::string& src, const std::string& destDir, bool overwrite = false);
#endif

    void importModel(const std::string& src, const std::string& destDir, bool overwrite = false);

    // Writes an edited material back to the .cs under Content\Materials that declares it. Returns
    // the file written, or "" with err set. GUARD IS ABOVE THE SIGNATURE, not inside the body (it
    // was, and `pbr::MaterialDesc` in the parameter list meant the function didn't compile with PBR
    // off). Content\Materials only, not Binaries\Materials -- unlike loadProjectMaterials()'s READ,
    // which honours both: Binaries holds avermatc's compiled
    // .ocmat, never the .cs source this edits -- a project with materials moved there and no .cs
    // left has nothing to write to, which the error below now says.
#if AVER_MODULE_PBR
    std::string saveMaterialSource(const std::string& name, const pbr::MaterialDesc& d, std::string& err);
#endif  // AVER_MODULE_PBR

    // GUARDED like saveMaterialSource just above it, and for the identical reason: the parameter
    // TYPE is pbr::MaterialHandle, which aver/pbr/Material.hpp declares and a PBR=OFF tree never
    // includes. It sat outside by one #endif.
#if AVER_MODULE_PBR
    void materialPanel(pbr::MaterialHandle handle);
#endif

    void buildModePanel(Engine& e);

    static bool editField(const char* label, std::string& value, usize cap);

    // panelFloat/panelInt moved to EditorWidgets.hpp (aver::editor namespace), no behaviour change,
    // so any editor can use the narrow-dock label-truncation fix. Call sites below say editor::panelFloat/editor::panelInt.

    void buildSelectModePanel();

    void buildSimulateModePanel(Engine& e);

    // Water is NOT a terrain feature: buildWaterPanel touches levelHeader_.waters/waves/water_,
    // names no landscape type, and its call site (level-properties panel, beside Sky/Fog) is guarded
    // on AVER_WITH_IMGUI alone -- see SandboxPanels.cpp's "WHERE THE GUARD STARTS" for the full case.
    // -DAVER_MODULE_LANDSCAPE=OFF is what made the disagreement a compile error.
    void buildWaterPanel(Engine& e);

#if AVER_MODULE_LANDSCAPE
    void buildLandscapeModePanel(Engine& e);

    void foliagePanelCreateType();

    void buildFoliageModePanel();
#endif

    void markLevelRecordEdited(bool& has);

    void buildPanels(Engine& e);

#if AVER_MODULE_SCENE
    // One Outliner row. Built by rebuildOutlinerCache() and kept in outlinerRows_ ACROSS frames --
    // see outlinerSignature() for what has to hold steady for the cache to stay valid. `par` is
    // the RAW engine parent; whether that parent is itself listed is a separate question, decided
    // in rebuildOutlinerCache()'s second pass.
    struct OutlinerRow { scene::Entity ent; scene::Entity par; std::string shown; };

    static std::string lowerCopy(std::string v);

    std::string outlinerLabelFor(scene::Entity e) const;

    void drawOutlinerDragSource(scene::Entity ent);

    void drawOutlinerDropTarget(scene::Entity ent);

    // `recordOrder` false: the caller has already put this row in outlinerOrder_ (a row the clipper
    // draws), so it must not be pushed again.
    void drawOutlinerRow(const OutlinerRow& row,
                         const std::unordered_map<u32, std::vector<const OutlinerRow*>>& children,
                         int depth, bool recordOrder = true);

    // A flat cache's rows, only the ones on screen (ImGuiListClipper); see the .cpp.
    void drawOutlinerFlatRows();
    // A tree cache's rows: the roots that have children and their open rows in full, the childless
    // roots between them only the ones on screen (a clipper per run); see the .cpp.
    void drawOutlinerTreeRows();

    // OUTLINER ROW CACHE, measured against a 12k+-entity import (Jungle Ruins: terrain tiles plus
    // thousands of alpha-masked foliage instances, no glass/water in it): buildOutlinerPanel used
    // to redo three passes over every entity PLUS a string-compare sort unconditionally, on every
    // single ImGui frame, even an idle one -- two std::string constructions per entity
    // (w.name(ent) just to test emptiness, then outlinerLabelFor(ent) again) and two UNRESERVED
    // hash containers that rehash repeatedly as they grow. outlinerRows_ owns the rows;
    // outlinerChildren_/outlinerRoots_ point INTO it, so rebuildOutlinerCache() always replaces
    // all three together (swaps a freshly-built vector into outlinerRows_) and never appends to
    // outlinerRows_ in place, which would invalidate those pointers.
    std::vector<OutlinerRow> outlinerRows_;
    std::unordered_map<u32, std::vector<const OutlinerRow*>> outlinerChildren_;
    std::vector<const OutlinerRow*> outlinerRoots_;
    // Found by the rebuild so the per-frame draws never dereference a row to learn them (the roots
    // are alphabetical, so their rows are scattered through outlinerRows_): the entity of each
    // outlinerRoots_ entry in the same order, and the ascending indices into outlinerRoots_ of the
    // roots that have listed children (empty for a flat list). The childless roots between two such
    // indices are what drawOutlinerTreeRows clips.
    std::vector<scene::Entity> outlinerRootEnts_;
    std::vector<u32> outlinerParentRoots_;
    // The signature the cache above was last built from, and whether it has been built at all --
    // kept separate from outlinerCacheSig_ because a default-constructed 0 is a value
    // outlinerSignature() can legitimately return, so it cannot double as "not built yet".
    u64  outlinerCacheSig_ = 0;
    bool outlinerCacheValid_ = false;

    // WHEN THE CACHE IS STALE, WITHOUT WALKING EVERY ENTITY EVERY FRAME (that walk was itself the
    // biggest cost of an idle 50k-entity level). Two halves, see the .cpp for what feeds each:
    //  - outlinerSignature(): an O(1) stamp of what the editor itself can see change (entity count,
    //    filter, label-map size, the level's entity list); refreshOutlinerCache() rebuilds when it
    //    moves.
    //  - outlinerAuditDiverged(): dense slots re-fingerprinted against outlinerSlotSigs_ (one
    //    fingerprint per dense slot, recorded by the rebuild): a SLICE per frame, so everything the
    //    stamp cannot see (a script rename, a reparent, a mesh added) is still caught within one
    //    audit cycle at a bounded cost per frame, and ALL of them the frame the editor's own edit
    //    stamp (outlinerEditMark) moves, so an edit made in the UI shows up at once.
    u64  outlinerSignature() const;
    u64  outlinerEditMark() const;
    u64  outlinerSlotSignature(u32 slot) const;
    bool outlinerAuditDiverged(bool full);
    // Rebuilds when either half above says the cache is stale; a no-op otherwise.
    void refreshOutlinerCache();
    // Redoes the three passes and the sort that used to run unconditionally in
    // buildOutlinerPanel every frame, filling outlinerRows_/outlinerChildren_/outlinerRoots_.
    void rebuildOutlinerCache();
    // True when the cache holds no parent/child pair (a level imported flat, tens of thousands of
    // scene ROOTS): every row is then a uniform-height leaf, so buildOutlinerPanel submits only the
    // rows on screen through ImGuiListClipper and outlinerOrder_ is built once by the rebuild. A tree
    // takes drawOutlinerTreeRows. Set by rebuildOutlinerCache, read by the panel.
    bool outlinerFlat_ = false;
    // One fingerprint per dense slot (World::at index) as of the last rebuild, and where the next
    // audit slice starts. Sized to the entity count the rebuild saw.
    std::vector<u64> outlinerSlotSigs_;
    u32  outlinerAuditCursor_ = 0;
    // outlinerEditMark() as of the last refresh, to tell a frame the editor issued a command in.
    u64  outlinerEditMark_ = 0;
    // ImGui::GetTime() of the last rebuild, and whether any Play is running: while one is, a rebuild
    // waits for half a second since the last (refreshOutlinerCache). False without the framework.
    double outlinerRebuiltAt_ = 0.0;
    bool outlinerPlaying() const;
#endif

    void buildOutlinerPanel();

    void buildDetailsPanel(Engine& e);

    void drawDrawer(Engine& e);

    void toggleDrawer(Drawer d);

    static ImVec4 notifyColour(editor::NotifySeverity s);

    void drawNotifications();

    void loadEditorPreferences();

    void resolvePreferredIdeFromPrefs();


    void buildEditorPrefs();
    // The centred Play group of the main toolbar: Play (repeats playMode_), the options dropdown,
    // Pause/Resume, Frame Skip, Eject/Possess, Stop. SandboxShell.cpp.
    void drawPlayToolbar(Engine& e);
    // Editor Preferences > Play, the page the dropdown's "Advanced Settings..." opens.
    // SandboxSettings.cpp.
    void buildPlayPrefsSection();

    void buildWorldSettings();

    void buildProjectSettings();

#if AVER_MODULE_VOXI
    static void featureStatusBadge(aver::voxi::Renderer& vx, aver::voxi::Feature f);
#endif

    // Window/Import/Streaming/Physics/Audio are NOT render pages -- landed behind AVER_MODULE_VOXI
    // only by proximity to buildRenderingSettings, the one page a voxel renderer actually touches.
    // buildSettings() dispatches all seven from one ImGui-gated else-if chain; a VOXI=OFF tree still
    // needs these five declared. settingInt, the -1-means-unstated int field three of those five
    // draw with, comes out with them.
    bool settingInt(const char* label, int* v, int lo, int hi, int whenEnabled, const char* tip);

    void buildWindowSettings();

    void buildImportSettings();

    void buildStreamSettings();

    void buildPhysicsSettings();

    void buildAudioSettings();

#if AVER_MODULE_VOXI
    void buildRenderingSettings(int page);
#endif

    void buildViewportOverlay();
    // The viewport's right-click menu (Play From Here). RMB is also fly-look, so it is ARMED on the
    // press and DECIDED on the release: only a click that stayed put opens it. SandboxViewport.cpp.
    void drawViewportContextMenu();
    struct ViewportCtx { bool armed = false; bool hasHit = false; Vec3 camPos{}; Vec3 hit{}; };
    ViewportCtx vpCtx_;
#endif

    // Requests the probe pixel and the screenshot on a capture run, then reports what was read.
    // --probe-rel resolves against the live viewport rect, --probe is absolute, neither means centre.
#if AVER_MODULE_SYNAPSE
    void navBakeCheck(Engine& e);
#endif

    void rayProbeCheck(Engine& e);

    void gpuTimingCheck(Engine& e);

    void resizeCheck(Engine& e);

    void captureCheck(Engine& e);

    void lumaSweepCheck(Engine& e);

    u64 maxFrames_; bool headless_; std::string beamPath_, shot_;
    u64 resizeCycle_ = 0;   // --resize-cycle N: 0 is off. See resizeCheck() for what it reproduces.
    bool gpuTiming_ = false;      // --gpu-timing: dump the per-pass GPU tree once, near the end
    bool gpuTimingDone_ = false;
    u32 resizeStep_ = 0;
    bool lumaSweep_ = false;          // --luma-sweep [STRIDE]: see lumaSweepCheck()
    int  lumaSweepStride_ = 1;        // frames between logged samples
    bool lumaSweepPending_ = false;
    u64  lumaSweepFrame_ = 0;
    f32  lumaSweepYaw_ = 0.0f;        // yaw_ cached at REQUEST time -- see lumaSweepCheck()
    f32  lumaSweepVpX_=0, lumaSweepVpY_=0, lumaSweepVpW_=0, lumaSweepVpH_=0;
    bool fireflyMetric_ = false;      // --firefly-metric [MULT]: see lumaSweepCheck()
    // Last sampled frame's luminance grid, so an outlier can be told from a FLICKERING outlier.
    std::vector<f32> fireflyPrevGrid_;
    u32 fireflyPrevW_ = 0, fireflyPrevH_ = 0;
    f32  fireflyMult_ = 8.0f;         // outlier threshold: local-neighbourhood-mean multiplier
    // MOVE, not Select: Select draws no gizmo (drawGizmo's tool_ test), so opening in Select gave a
    // freshly picked object nothing to grab and no hint that 2 would summon one -- "I cannot move
    // things" was accurate, not a misunderstanding. Unreal keeps a gizmo up too. 1 and --tool both
    // still override this.
    Tool initialTool_ = Tool::Move;
    std::vector<MeshObj> objects_;
    // The selection addresses either world: sel_ >= 0 is an objects_ index, -1 is nothing,
    // -2/-3/-4 are the sun/sky/post pseudo-entries, kSelScene means selEntity_ names a scene entity.
    static constexpr int kSelScene = -5;
    int sel_ = 1;
    // AvId, not scene::Entity: read and written from unguarded code (pick() across both worlds, the
    // sun/sky/post-process outliner rows) as a generic "nothing selected" sentinel. scene::Entity is
    // a bare alias for AvId, so this drops the dependency with no behaviour change.
    AvId selEntity_ = kInvalidId;
    bool hideEditorScene_ = false;
    rhi::IDevice* prefsDevice_ = nullptr;   // borrowed, latched in buildUI
    std::string prefIdeName_;               // stored IDE name, pending the async scan that resolves it
    bool prefsLoaded_ = false;
    // Set by loadLevel when a level supplied its own CAMERA record; read by onAttach so the default
    // framing does not overwrite it. See both sites.
    bool levelCameraRestored_ = false;
    editor::AssetEditorHost assetEditors_;
    bool vsyncOffRequested_ = false;        // --no-vsync, pending a device to apply it to
    bool vsyncOnRequested_ = false;         // --vsync, the same, the other way
    bool wantMeshReload_ = false;
    // Outliner display names. Not scene::World::name(), which holds the asset path.
    std::unordered_map<u32, std::string> entityLabels_;
    std::unordered_map<std::string, int> labelCounts_;
    std::unordered_map<u32, int32_t> entityBodies_;   // the body an entity owns (kinematic if animated, else static)

    static constexpr std::size_t kUndoDepth = 128;
    std::vector<EditCmd> undoStack_, redoStack_;
    std::unordered_map<EditId, AvId> editToEntity_;
    std::unordered_map<u32, EditId> entityToEdit_;
    EditId nextEditId_ = 1;
    EditXform editBefore_{};
    bool editBeforeValid_ = false;
    // The rest of a multi-selection's LOCAL transforms as they were when the drag began. Only
    // non-empty between beginTransformEdit and endTransformEdit; see EditCmd::alsoMoved.
    std::vector<std::pair<EditId, Transform>> multiMoveBefore_;

    // Copy/Duplicate's source, what Paste rebuilds from. `entities`/`hasObject` are set exclusively
    // by copySelection() -- mirrors the loose sel_/selEntity_ pairing rather than a variant type.
    // ONE COPIED ENTITY, split out of EditorClipboard so the clipboard can hold a LIST: Ctrl+C used
    // to read the anchor alone, so copying five props and pasting produced one -- the same
    // "applies to the set, acts on the anchor" shape that made multi-move unundoable, the bug Ctrl+D had.
    struct ClipboardEntity {
#if AVER_MODULE_SCENE
        editor::EntitySnapshot snap;
        // EVERYTHING UNDER IT, TOO. Copying only the entity the selection names meant pasting a
        // parent produced a childless copy -- silently, since the paste looked like it worked.
        std::vector<EditCmd::DestroyedNode> subtree;
#endif
        EditXform xform{};
        bool hadBody = false;   // see EditCmd::hadBody's own comment -- no shape rides along with it
        // The root's non-component flags and object animation, as EditCmd carries them (its children
        // already keep theirs through `subtree`): without these a pasted/duplicated ROOT came back
        // solid, unsnapped and unanimated.
        bool  hadCollide = true;
        bool  hadSnapZ   = false;
        f32   snapZ      = 0.0f;
        EntityAnim hadAnim;
    };

    struct EditorClipboard {
        // Empty means nothing was copied -- this replaces the old hasScene bool outright.
        std::vector<ClipboardEntity> entities;

        // THE PLACEHOLDER PATH STAYS SINGLE-ITEM: objects_/MeshObj (no-project path) has no
        // multi-selection concept -- multiSel_ lives behind AVER_MODULE_SCENE, so there's no set to copy.
        bool hasObject = false;
        MeshObj object{};
    };
    EditorClipboard clipboard_;

#if AVER_MODULE_PBR
    // One material edit in flight: the desc as it was before this interaction began, and which
    // material it belongs to. See materialPanel for why `before` is snapshotted at the top of a
    // frame rather than when a control reports it was activated.
    bool matEditActive_ = false;
    bool matEditDirty_  = false;
    pbr::MaterialHandle matEditHandle_ = 0;
    pbr::MaterialDesc   matEditBefore_{};
#endif

    // What chord means what command, defaults matching every hardcoded key this file used before
    // this registry existed. See EditorKeybinds.hpp for why it lives in its own file.
#if AVER_WITH_IMGUI
    // A REFERENCE to the one registry, not an instance: asset-editor tabs reach the same object via
    // editor::keybinds(); two registries would mean a Preferences-page rebind silently missing tabs
    // -- the "rebindable unless you are in a tab" split this promotion removes.
    editor::KeybindRegistry& keybinds_ = editor::keybinds();
#endif

    std::string makeEntityLabel(const std::string& surface, const std::string& asset);
    // The word makeEntityLabel numbers ("Wood" for M_Wood, else the asset's stem, else "Entity"). Split
    // out so saveLevel can tell a generated label from a renamed one without advancing labelCounts_.
    static std::string entityLabelBase(const std::string& surface, const std::string& asset);
    Tool tool_ = Tool::Move;   // see initialTool_ for why this is Move and not Select

    // Which mode the viewport is in. UNGUARDED even though sculpting is AVER_MODULE_LANDSCAPE-only:
    // the mode switch/viewport hint/input dispatch all read it unconditionally; Landscape just never
    // becomes reachable when the module's off.
    EditorMode mode_ = EditorMode::Select;
#if AVER_MODULE_LANDSCAPE
    SculptTool sculptTool_ = SculptTool::Raise;
    // 1.0 is the smoothstep the brush always had; see BrushParams::falloff.
    f32 sculptFalloff_ = 1.0f;

    // FOLIAGE: one entry per .ocfoliage TYPE ASSET in the project's content folder, not per mesh.
    // What a species places (mesh, material, scale range, weight, randomizeYaw, collisionRadiusCm,
    // alignToNormal) lives in the loaded fmt::OcFoliageData, authored from its own FoliageTypeEditor
    // tab, rather than ad hoc fields shared by the whole palette -- see OcFoliage.hpp for why this
    // makes a foliage type a reusable asset, not a global brush setting. Empty no longer refuses
    // mode entry (editor::foliageModeGate); it
    // means buildFoliageModePanel() shows a create-a-type empty state instead of the palette list.
    struct FoliageSpecies {
        std::string name;         // display name, the file stem
        std::string assetPath;    // the .ocfoliage this came from -- opened by the palette's Edit button
        fmt::OcFoliageData type;  // mesh/material/scale/weight/randomizeYaw/collisionRadiusCm/alignToNormal
        bool         enabled = true;
    };
    std::vector<FoliageSpecies> foliagePalette_;
    // BRUSH-ONLY settings: how the tool is held, not what it places -- everything about WHAT gets
    // placed and how each instance randomises now lives per type above.
    f32  foliageRadiusCm_    = 800.0f;
    f32  foliageDensity_     = 6.0f;     // attempted placements per brush application
    bool foliageErase_       = false;    // Shift: remove instead of place
    u32  foliageSeed_        = 1u;       // one running RNG stream for the whole paint session
#endif

    bool editorModeIsLandscape() const;

    bool editorModeAvailable(EditorMode m, const char** whyNot = nullptr) const;

    bool editorModeIsFoliage() const;

    void setEditorMode(EditorMode m);

    void toggleEditorMode();
    EditorMode lastNonSelectMode_ = EditorMode::Landscape;
    std::string startMode_;

    void applyStartMode();
    // Free-fly editor camera: position plus yaw/pitch.
    Vec3 camPos_{7.0f, 7.0f, 4.5f};
    f32 yaw_ = 0.0f, pitch_ = 0.0f, flySpeed_ = 800.0f, lookSpeed_ = 0.005f;   // cm/s

    // What "1" on the camera-speed dial means, in cm/s. flySpeed_ stays in cm/s (movement
    // integration and the saved preference both use it; changing the unit would reinterpret every
    // editor.ini). DISPLAY scale only: the chip/slider divide by it, so default speed reads "1" not
    // "800" -- tunable by feel, like Unreal's 1-8 dial, not a raw rate to convert in your head.
    static constexpr f32 kCamSpeedUnit = 800.0f;
    bool flying_ = false;
    // The pawn viewed in FIRST PERSON this frame, or kInvalidEntity -- set by drivePlayCamera(),
    // read by the owner-hide check beside frustum/occlusion culls: a mesh with
    // kMeshRendererHiddenFromOwner skips its rasterised draw when this is itself or an ancestor
    // (untouched for third-person, which uses a boom offset).
    // RESET UNCONDITIONALLY at the top of drivePlayCamera(), never left stale across an early
    // return -- same bug shape as aver-float-cannot-hold-handles: a stale handle matching a reused one.
    // GUARDED: scene::Entity doesn't exist with AVER_MODULE_SCENE=OFF; this sat unguarded between
    // flying_ and the selection-outline latch by pure proximity to the camera members. Both use sites
    // are already inside a scene guard (SandboxRender.cpp's ownerHideRoot) or a framework one
    // (SandboxPlay.cpp's drivePlayCamera; FRAMEWORK is forced off with SCENE).
#if AVER_MODULE_SCENE
    scene::Entity firstPersonPawn_ = scene::kInvalidEntity;
#endif
    // Latched during the scene pass so the outline draws after every surface is down.
    Mat4 selectionOutline_{}; rhi::MeshHandle selectionMesh_ = 0; bool hasSelection_ = false;
    // Every selected entity's (world transform, mesh id) for this frame's outline pass. Rebuilt
    // during the draw walk and cleared right after drawing, so it can never describe a stale set.
    std::vector<std::pair<Mat4, u64>> selectionOutlines_;
    // Top-of-atmosphere colour, mirroring rhi::SkyAtmosphere::sunColor's own default -- white,
    // since the physical sky tints it by elevation on its own.
    f32 sunColor_[3]={1.0f,1.0f,1.0f}, sunAmbient_=1.0f;
    // THESE NUMBERS MUST STAY THE FORMAT'S: the editor's mirror of sky_.zenith/horizon, copied OVER
    // sky_ every frame (SandboxApp.cpp:2692), so rhi::SkyAtmosphere's defaults (RHI.hpp:253) never
    // survive frame 1 -- a level with no SKY record renders whatever is written here (applyLevelEnv
    // is guarded on w.hasSky, LevelSky.hpp:56). Used to read (0.19,0.42,0.78)/(0.72,0.80,0.90), the
    // odd one out: OcWorld.hpp:401's format defaults are (0.24,0.45,0.85)/(0.72,0.83,0.95), matching
    // GameApp.hpp:420/RHI.hpp:253 -- moved to match, since "the editor is the behaviour reference"
    // (docs/RUNTIME-DEDUP.md) can't hold against a value the format itself declares. CHANGES PIXELS for a no-SKY level under
    // the AUTHORED sky model (--sky-authored, Sky panel, or skyPhysical=false); under Physical the
    // dome fit overwrites both anyway (D3D12Device.cpp:4211, VulkanDevice.cpp:2356), which is why
    // this sat wrong unnoticed.
    f32 skyZenith_[3]={0.24f,0.45f,0.85f}, skyHorizon_[3]={0.72f,0.83f,0.95f};
    // A tint on the in-scattered sky (white = clear air), and an extinction per cm.
    f32 fogColor_[3]={1.0f,1.0f,1.0f}, fogDensity_=4e-6f;
#if AVER_MODULE_SCENE
    // OPT-IN (Height Fog panel, shown only while chunk streaming is on): recomputes fogDensity every
    // frame from the streaming load boundary rather than a fixed density drifting out of sync (see
    // the per-frame fog push for the derivation: it solves averFogFactor's k<=1e-8 branch exactly,
    // not an approximation). Default OFF: visibly foggier,
    // must never become the silent default.
    bool matchFogToStreamRadius_ = false;
    f32  fogMatchTargetOpacity_ = 0.9f;   // opacity WANTED at the load boundary itself
#endif
    rhi::PostSettings post_{};
    // What this editor last handed the device, so the per-frame push can tell when something ELSE
    // (console post.* vars) changed it in between and adopt that instead of overwriting it. Invalid
    // until the first push, so frame 1 never mistakes device defaults for an edit.
    rhi::PostSettings postPushed_{};
    bool postPushedValid_ = false;
    // Which post values came from argv, so a stored preference cannot silently outrank a flag the
    // caller typed. Set by setPost, read once by loadEditorPreferences.
    bool postExposureFromCli_ = false, postBloomFromCli_ = false, postAutoExpFromCli_ = false;
    // The authored sky, sun and air. Sole owner of the sun's direction.
    rhi::SkyAtmosphere sky_{};
    f32 cloudTime_ = 0.0f;   // seconds of accumulated wind
    bool capDone_=false;
    // The pixel requested and the viewport rect it was requested against, latched at request time.
    u32 capX_=0, capY_=0;
    f32 capVpX_=0, capVpY_=0, capVpW_=0, capVpH_=0;
    // editor viewport aids
    rhi::LineHandle gridMesh_=0;
#if AVER_MODULE_SYNAPSE
    // The baked grid and the line mesh drawn from it. Mesh is REBUILT on every bake/load, which is
    // why destroyLineMesh had to exist first: without it each rebuild leaked an upload buffer.
    fmt::OcNavData  nav_;
    rhi::LineHandle navMesh_=0;
    bool showNav_=false;
    bool navRegionColours_=true;
    // --bake-nav: bake once at startup, then carry on. Deferred to a frame rather than done at
    // init because the bake reads PHYSICS BODIES, and a level's bodies are built by
    // applyProject, which has not run when the app is constructed.
    bool navBakeOnStart_=false;
    bool navBakeDone_=false;
    bool navLoadPending_=false;
    f32  navBakeCell_=50.0f;
#endif

    // ---- the state belonging to the three non-navigation features above ----
    // Collider overlay: the world-space AABB of every physics body (no prior way to see collision
    // at all: no toggle, no wireframe, nothing). TOGGLE UNGUARDED, MESH NOT: the toggle is "a
    // property of the VIEW and not of the level", persisted through
    // editor.ini (loadEditorPreferences/saveEditorPreferences run in every configuration, so it
    // can't only exist when the solver does); the line mesh is what's genuinely built from bodies.
    bool showColliders_=false;
#if AVER_MODULE_PHYSICS
    // Two meshes: colliderMesh_ the STATIC bodies, colliderMoverMesh_ the kinematic and dynamic ones
    // (see rebuildColliderOverlay's .cpp comment for why they are separate).
    rhi::LineHandle colliderMesh_=0;
    rhi::LineHandle colliderMoverMesh_=0;
    // What colliderMesh_ was built from, so rebuildColliderOverlay can skip an unchanged frame (see
    // its .cpp comment). Built = the mesh reflects colliderOverlaySig_; AllKnown = every STATIC physics
    // body was one the editor tracks (levelBodies_/entityBodies_), so the sig covers all the ways the
    // static set can change. colliderRev_ is bumped wherever the editor remakes or drops a body of its
    // own (rebuildEntityBody, destroyEntity).
    bool colliderOverlayBuilt_ = false;
    bool colliderOverlayAllKnown_ = false;
    u64  colliderOverlaySig_ = 0;
    u64  colliderRev_ = 0;
    // The moving mesh's state. colliderMovers_ = the non-static handles, captured whenever
    // colliderMesh_ is rebuilt; colliderMoversTracked_ = the editor tracks every one of them (so,
    // outside Play, none can move unseen); colliderMoversWereLive_ = a play session ran at the last
    // call (one more refresh when it ends); colliderMoverBoxes_ = the AABBs (lo xyz, hi xyz per
    // mover) colliderMoverMesh_ was built from, compared with colliderMoverBoxesNext_ (scratch) to
    // skip a frame where nothing moved; colliderMoverLines_ = scratch for the mesh's vertices.
    std::vector<int32_t> colliderMovers_;
    std::vector<f32> colliderMoverBoxes_, colliderMoverBoxesNext_;
    std::vector<rhi::LineVertex> colliderMoverLines_;
    bool colliderMoversTracked_ = false;
    bool colliderMoversWereLive_ = false;
    // The static audit: the handles colliderMesh_ drew, with the AABB each was drawn at (lo xyz, hi xyz,
    // parallel to colliderStatics_). In Play a rotating slice of them is re-read each frame, and any
    // that is no longer static or has moved forces one full rebuild (a script's SetBodyMotionType or
    // body teleport changes neither the body count nor colliderRev_). colliderAuditCursor_ = where the
    // next slice starts.
    std::vector<int32_t> colliderStatics_;
    std::vector<f32> colliderStaticBoxes_;
    usize colliderAuditCursor_ = 0;
#endif
    // Frame time and the Play-only CPU phases, fed by onUpdate's begin()/end() brackets and folded
    // once a frame in the status bar. Unguarded: onUpdate brackets it in every configuration.
    editor::PlayProfile playProf_;
#if AVER_WITH_IMGUI
    // The profiler panel (Window > GPU Profiler): the GPU pass table, a view over GpuTimingReport
    // that the device has always produced and only the console used to read, above which sit
    // playProf_'s frame time and Play CPU phases.
    bool showProfiler_=false;
    // The status bar's VRAM readout (SandboxShell.cpp): the last videoMemory() answer, polled twice a
    // second, and whether the over-budget warning has been logged for the current crossing.
    rhi::VideoMemoryInfo vram_{};
    f32  vramPollS_ = 0.0f;
    bool vramOverLogged_ = false;
    // Window > Neural Visualiser. The choices live here (not in the prefs) and are applied every frame
    // by onUpdate: neurafiViz_'s source/opacity, NeuraFI::setVisualisation, the device's
    // generated-only view, and VoxiRenderer::setNeuRaCView. Closing the window does not turn them off --
    // its "Off" entries do -- so a view can stay up while the window is docked away.
    bool showNeuralViz_ = false;
    int  neurafiVizMode_ = 0;          // neurafi::Visualisation
    f32  neurafiVizOpacity_ = 0.85f;
    f32  neurafiVizScalePx_ = 0.5f;    // path bend at full heat
    bool neurafiShowGeneratedOnly_ = false;
    int  neuracViewMode_ = 0;          // VoxiRenderer::setNeuRaCView's mode
    bool neuracViewGrid_ = false;
    // The References panel. refPanelScanned_ distinguishes "opened but never asked" from "asked and
    // found nothing" -- two states an empty list cannot tell apart, and the second is the useful one.
    bool showReferences_ = false;
    bool refPanelScanned_ = false;
    std::string refPanelAsset_;
    std::vector<std::string> refPanelResults_;
#endif
    rhi::LineHandle gzMove_[3]={0,0,0}, gzMoveHi_[3]={0,0,0};
    rhi::LineHandle gzRot_[3]={0,0,0}, gzRotHi_[3]={0,0,0};
    rhi::LineHandle gzScale_[3]={0,0,0}, gzScaleHi_[3]={0,0,0};
    bool showGrid_=true, wireframe_=false;
    // gizmo interaction
    bool dragging_=false; int activeAxis_=-1, hoverAxis_=-1;
    // Rotation drag accumulators, so rotation snap works on the total swept angle
    // rather than on a per-frame delta that would round to nothing. See applyRotate.
    f32 rotDragDeg_=0.0f, rotAppliedDeg_=0.0f;
    f32 prevMouseX_=0, prevMouseY_=0;
    // snapping (off by default; toggled from the toolbar carets)
    bool snapMove_=false, snapRot_=false, snapScale_=false;
    f32 moveSnap_=1.0f, rotSnap_=15.0f, scaleSnap_=0.25f;
    // frame state
    f32 dpi_=1.0f;
#if AVER_WITH_IMGUI
    ImFont* fontMedium_=nullptr; // Roboto Medium, for the menu bar; null if only the fallback loaded
    // Editor's Dear ImGui backend, installed into the device non-owning -- same unique_ptr-owns/
    // raw-pointer-on-the-device shape as averSrUpscaler_, but UNLIKE that one, onShutdown does NOT
    // reset() this early: the device's uiShutdown() (between onShutdown returning and `delete app`)
    // needs it still alive, or resetting early reproduces the same dangling-pointer bug sooner.
    std::unique_ptr<rhi::d3d12::IUiBackend> uiBackend_;
#endif
#if AVER_WITH_IMGUI_VULKAN
    // The Vulkan one, held on exactly the same terms and for the same teardown reason as the D3D12
    // member above: the device's own raw pointer must not outlive this, and natural destruction
    // order is what keeps that true with no explicit detach.
    std::unique_ptr<rhi::vkb::IUiBackend> uiBackendVk_;
#endif
    // 3D viewport rect, in backbuffer pixels. Latched by buildUI, consumed the next frame.
    f32 vpX_=0, vpY_=0, vpW_=1600, vpH_=900;
    bool dockBuilt_=false;   // false = build the default layout on the next frame
    // Set by View > Reset Layout so the builder overrides a layout restored from the ini.
    // Without it, Reset would clear dockBuilt_ and the adopt-restored branch would immediately
    // set it again -- the menu item would do nothing at all.
    bool dockResetRequested_=false;
    // Warn ONCE about a missing icon font, not once per DPI change: applyDpi re-runs on every
    // monitor change, and a per-change warning would fill the log for one cosmetic asset.
    bool iconWarned_=false;
    bool showProjectSettings_=false; // Edit > Project Settings window
    bool showWorldSettings_=false;   // Window > World Settings (per-LEVEL settings)
    // Window > World Outliner / Details. Default ON: these are the editor's two primary panels and
    // hiding one is the deliberate act, not showing it.
    bool showOutliner_=true;
    bool showDetails_=true;
    // Viewport Show flags. Both default ON -- hiding is the deliberate act.
    bool showStaticMeshes_=true;
    bool showAtmosphere_=true;
    // File > Save Level As...
    bool unlit_=false;                 // View mode: Unlit (no shading, authored colour only)
    bool showAbout_=false;             // Help > About
    bool wantSaveLevelAs_=false;
    char saveLevelAsName_[128]={};
    std::string saveLevelAsError_;
    bool showEditorPrefs_=false;     // Edit > Editor Preferences window
    bool scrollPrefsToKeybinds_=false;   // --scroll-prefs-to-keybinds, one-shot
    int  settingsPage_=1;            // 0 Description, 1 Rendering>General, 2 >GI, 3 >Ray Tracing, 4 >Path Tracing
    int  focusVoxi_=0;               // --project-settings: frames left to force the window open
    int  msaaOverride_=0;            // --msaa N: apply a sample count at startup
    // --depth-prepass: same-frame depth-only pass ahead of the ordinary opaque colour walk, so an
    // occluded fragment never reaches PSMainVoxi's shadow lookup/cone trace/fog. OFF (default) never
    // calls setDepthPrepassEnabled/drawMeshDepthPrepass/setNextDrawPrepassed (see renderSceneEntities).
    bool depthPrepassOverride_ = false;
    // --gbuffer: see GBufferDebugFeature's top comment / onUpdate's setGBufferEnabled push. A plain
    // override, not applied directly: the debug view below must also enable it on its own, OR'd
    // together every frame.
    bool gbufferOverride_ = false;
    // --gbuffer-debug velocity|viewz|normals, or the matching view-mode dropdown entries: which
    // channel (if any) GBufferDebugFeature draws over the 3D viewport. Off costs one enum compare.
    // Makes gBufferVelocityTexture()/gBufferViewZTexture()/gBufferNormalRoughnessTexture() probeable (captureCheck()).
    GBufferDebugFeature::Mode gbufferDebugView_ = GBufferDebugFeature::Mode::Off;
    // Registered once in onInit, unregistered in onShutdown (both unconditional -- this is generic
    // RHI, gated on no module). Never rebuilt: one instance for the whole run, exactly like
    // voxiRenderer_ below.
    GBufferDebugFeature gbufferDebugFeature_;
    bool gbufferDebugAttached_ = false;
    // Window > Neural Visualiser's NeuraFI overlay. Registered beside gbufferDebugFeature_, for the run.
    NeuraFiVizFeature neurafiViz_;
    // --occlusion-cull: hierarchical-Z two-pass box culling (modules/occlusion). OFF (default) never
    // calls occluder_ or reorders the entity walk -- see renderSceneEntities for the in-place
    // two-pass reorder (reuses the draw logic depthPrepassOverride_'s walk doesn't replicate).
    // UNGUARDED, alone on this side of the #if, because it's a MANIFEST MIRROR first:
    // RENDER.OCCLUSIONCULL round-trips through this bool (applyProjectRenderSettings,
    // captureRenderSettingsFromUi), both gated on VOXI not this module -- an OCCLUSION=OFF tree once
    // lost the member while the round-trip kept using it (a compile error); guarding the uses
    // instead would be worse: an OCCLUSION=OFF build would silently rewrite a teammate's
    // OCCLUSIONCULL 1 to 0 on every settings edit. Everything below (the culler itself, its
    // per-entity bookkeeping) stays guarded.
    bool occlusionCullEnabled_ = false;
// AND AVER_MODULE_SCENE, not just OCCLUSION -- every other `#if AVER_MODULE_OCCLUSION` here carries
// the same pair: two members below key on scene::Entity, so OCCLUSION-on/SCENE-off named a
// nonexistent type (module-matrix's scene-off/all-off rows failed here). Written at all nine sites,
// not centrally, since none nests inside one SCENE region -- guarding only the members would leave
// readers compiling against vanished ones, a shape this file's been bitten by before.
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
    bool occlusionCullForceOff_ = false;   // --no-occlusion-cull: see setOcclusionCullForceOff's own comment
    // --occlusion-waitidle / --no-occlusion-waitidle: see setOcclusionDebugForceWaitIdle. CLI-seeded
    // default only -- once occluder_ exists, EditorConsole.hpp's consoleOcclusionForceWaitIdleSlot()
    // (seeded from this in onInit) is the live source of truth, reasserted onto occluder_ every frame
    // in onUpdate, so a console `set occlusion.debugForceWaitIdle` takes effect the same way either
    // flag does. DEFAULTS TRUE: the no-wait path measured worse without a found cause (see
    // OcclusionCuller.cpp's kInFlight FOLLOW-UP); --no-occlusion-waitidle opts back into the faster,
    // unproven-correct path.
    bool occlusionDebugForceWaitIdleArg_ = true;
    // NON-owning would be wrong here: this module has no registry of its own the way IRenderFeature
    // does, so SandboxApp owns the one instance for the run and destroys it in onShutdown.
    aver::occlusion::IOcclusionCuller* occluder_ = nullptr;
    // Per-entity "the pyramid could not prove this hidden, as of last test" bit, carried across
    // frames (bookkeeping belongs to the CALLER, not the culler -- Occlusion.hpp). Absent = never
    // tested = visible, so a freshly spawned entity is never missing from its first frame.
    std::unordered_map<scene::Entity, bool> occlusionVisible_;
    // This frame's reordering of [0, w.count()) so every PASS-1 (assumed-visible) index precedes
    // every PASS-2 one (renderSceneEntities). A MEMBER, not a local, to reuse the allocation frame to
    // frame rather than reallocating a several-thousand-entry vector every frame.
    std::vector<u32> occlusionOrder_;
    // This frame's world AABBs, collected in a pre-walk ahead of the main loop: the culler needs
    // ALL of them (not just pass-2) in one call, computed once up front rather than reusing the main
    // loop's per-pass-2-entity box. Parallel arrays, kept as members like occlusionOrder_.
    std::vector<aver::occlusion::Aabb> occlusionBoxes_;
    std::vector<scene::Entity> occlusionBoxEntities_;
    std::vector<u8> occlusionResults_;
    // Diagnostics: accumulated since the process started, and the frame count they cover -- see the
    // periodic log line in renderSceneEntities for where these are read and reset.
    u64  occlusionCulledAccum_ = 0, occlusionTestedAccum_ = 0;
    u32  occlusionReportFrames_ = 0;
    bool occlusionWasVisible(scene::Entity e) const;
    // ---- MOTION-SAFE CULLING: the camera basis testBatch()'s answer was actually computed from
    // (at best one-frame-stale) -- see renderSceneEntities' trust-gate comment and Occlusion.hpp's
    // corrected "TWO-PASS" section for why an answer needs this. Stashed after buildPyramid() runs
    // (inside occlusionBuildAndTest), the same "camera value as of last look" idiom chunk streaming
    // uses, reused rather than adding a second accessor to IOcclusionCuller (bookkeeping belongs to
    // the CALLER, not the culler).
    Vec3 occlusionBasisCamPos_{0.0f, 0.0f, 0.0f};
    Vec3 occlusionBasisForward_{1.0f, 0.0f, 0.0f};
    bool occlusionBasisValid_ = false;
    // F7: the scene's sub-rect (target pixels) that produced THIS basis's pyramid, stashed for the
    // same reason as occlusionBasisCamPos_/Forward_: a dock-layout drag between build and readback
    // is a discontinuity like a teleport. occlusionTrustworthy requires this to equal this frame's
    // sceneViewport().
    f32 occlusionBasisRect_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // F7's once-per-change diagnostic: last rect/pyramid size the "[Occlusion] testing against scene
    // rect..." line printed for, so a static layout logs once, not every frame. occlusionLoggedPyramidW_
    // starts at a value no real texture width equals, so the first frame culling turns on always logs.
    f32 occlusionLoggedRect_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    u32 occlusionLoggedPyramidW_ = 0xFFFFFFFFu, occlusionLoggedPyramidH_ = 0xFFFFFFFFu;
    // F8: has the "[Occlusion] idle: ..." line already fired for the CURRENT idle streak. Reset to
    // false the moment occlusionRuns is next true, so idle -> running -> idle logs a second time
    // rather than only ever once per process.
    bool occlusionIdleLogged_ = false;
    // 3B: this frame's copy of the two EditorConsole.hpp slots, reasserted every frame in onUpdate
    // so onRender's walk reads a value live for THIS frame, not last frame's. Both default false,
    // matching the slots' own defaults.
    bool occlusionShowCulled_ = false;
    bool occlusionCullUnderSuppression_ = false;
    // Diagnostics for the staleness detector (OcclusionCuller.cpp's generation-stamp check, via
    // IOcclusionCuller::readbackLagIsExactlyOneCall()): frames it fired on, and whether the one-time
    // loud warning already fired. Folded into the periodic report with occlusionCulledAccum_.
    u64  occlusionStaleReadbacks_ = 0;
    bool occlusionStaleWarnedOnce_ = false;
#endif
    // All three use -1 for "flag not given", NOT 0 (0 = Quality::Off, must be expressible). Used to
    // be 0, so --gi 0/--rt 0/--pt 0 were silent no-ops through both the startup path and
    // applyProjectRenderSettings; --no-gi/--no-rt were workarounds, PT never got one.
    int  giOverride_=-1;             // --gi [tier]: GI quality to apply at startup
    bool giForceOff_=false;          // --no-gi: force it off, whatever the default is
    int  rtOverride_=-1;             // --rt [tier]: ray tracing quality at startup
    int  ptOverride_=-1;             // --pt [tier]: PATH tracing quality at startup
    bool rtForceOff_=false;          // --no-rt: force it off, whatever the default is
    int  rtRaysOverride_=0;          // --rt-rays N: sun occlusion rays per pixel (0 = flag not given)
    // --gi-sky-occlusion-rays N: AMBIENT sky-visibility rays per pixel. -1 = flag not given, because
    // 0 is a real value here (fall back to the cone gather's own occlusion).
    int  giSkyOccRaysOverride_=-1;
    int  giSkyOccTileOverride_=-1;   // --gi-sky-occlusion-tile N
    // --sky-light N. NEGATIVE means absent: 0 is a real, meaningful request (no sky ambient at all),
    // which is exactly the measurement this flag was added to make possible.
    f32  skyLightOverride_=-1.0f;
    // --gi-intensity F. Negative means absent; 0 is a real request (bounce off, direct only).
    f32  giIntensityOverride_=-1.0f;
    // --sky-physical / --sky-authored: -1 absent, 0 authored, 1 physical. And the sun elevation in
    // degrees, -999 when absent (a real elevation can legitimately be negative -- the sun below the
    // horizon is night, not an unset value).
    int  skyModelOverride_=-1;
    f32  sunElevationOverride_=-999.0f;
    int  rtPixelsPerRayOverride_=0;  // --rt-pixels-per-ray N: shadow tile edge (0 = flag not given)
    int  rtShadowDenoiseOverride_=-1; // --rt-shadow-denoise N: spatial radius (-1 = flag not given)
    int  rtRenderModeOverride_=-1;    // --rt-render-mode 0|1 (-1 = flag not given)
    int  rdStagesOverride_=-1;        // --rd-stages 0|1|2 (-1 = flag not given)
    int  ptBouncesOverride_=-1;       // --pt-bounces N (-1 = flag not given)
    int  layeredBsdfOverride_=-1;     // --layered-bsdf N (-1 = flag not given)
    f32  coatWeight_=0.0f;            // --coat W [R] [F0]: 0 = flag not given, and no coat anywhere
    f32  coatRough_=0.1f;
    f32  coatF0_=0.04f;
    int  giUpdateIntervalOverride_=0; // --gi-update-interval N: GI revoxelise interval (0 = flag not given)
    // --gi-mode N: indirect-diffuse estimator, 0 = voxel cones, 1 = ReSTIR GI (Settings::giMode).
    // Sentinel is -1, not 0 like its neighbour above: 0 ("voxel cones") is a real value here, so the
    // giUpdateIntervalOverride_ convention would make `--gi-mode 0` indistinguishable from unset.
    int  giModeOverride_=-1;
    // --restir-visibility none|reconstructed|half|full: optimisation-wave-2's U1 (Settings::
    // giRestirVisibility). Same -1-is-absent sentinel as giModeOverride_: 0 (NoRay) is a real value,
    // not "flag not given".
    int  restirVisibilityOverride_=-1;
    int  denoiserOverride_=-1;       // --denoiser 0|1: -1 is "flag not given"; see setDenoiser
    f32  renderScaleOverride_=1.0f;  // --render-scale F: scene render resolution as a fraction of present, clamped [0.25,1]
#if AVER_MODULE_SR
    // --aversr LEVEL / the render-settings quality combo. Off (default) = no AverSR: no render-scale
    // change beyond --render-scale, no SpatialUpscaler construction. See docs/AVERSR.md and
    // onInit()/applyAverSrQuality() for where it's read.
    aver::sr::Quality averSrQuality_ = aver::sr::Quality::Off;
    // True when --aversr set the value above, so loadEditorPreferences must not let a stored
    // preference overwrite the CLI flag (persisted state loses to a flag everywhere in this file).
    // Also true for --aversr auto (setAverSrCliAuto): CLI still wins even though auto has no single
    // level to pin -- averSrCliLevel_ stays -1 and updateAverSrAuto lets the manifest/ladder chain
    // decide the level every frame instead, per --aversr auto's "preferences are ignored; manifest
    // and ladder apply" contract (plan section 3.3 A).
    bool averSrFromCli_ = false;
    bool averSrCliAuto_ = false;      // --aversr auto: see averSrFromCli_'s own comment just above
    int  averSrCliLevel_ = -1;        // --aversr LEVEL (not auto): the pinned level, for
                                       // updateAverSrAuto's resolveAverSrLevel call every frame
    // optimisation-wave-2, U2/3.3 A: the user's own Display preference, superset of averSrQuality_
    // (Auto and Manual besides the four named levels -- see AverSrChoice.hpp's own top comment for
    // why aver::sr::Quality alone cannot carry either). Loaded once from display.aversrChoice
    // (migrated via migrateAverSrChoice if absent), written by the Display combo, read every frame by
    // updateAverSrAuto as the "user" rung of the CLI > user > manifest > auto chain.
    editor::AverSrChoice averSrChoice_ = editor::AverSrChoice::Auto;
    // Set once, the session a stored display.renderScalePending cookie is found still armed at load
    // (3.3 A's own "a named level did not survive its own launch" case, the non-Manual sibling of
    // the pre-existing renderScaleCookieArmed_ dance) -- forces Off for the REST OF THIS SESSION
    // regardless of averSrChoice_/manifest/ladder, so a crashing level is never silently
    // re-attempted. Cleared by nothing; only a fresh launch re-arms it.
    bool averSrCookieTripped_ = false;
    // Set true only when migrateAverSrChoice ran on a genuinely ABSENT display.aversrChoice key and
    // landed on Auto -- first session after migrating into U2's new default. Cleared the moment the
    // user picks any item on the Display AverSR combo or the Project Settings "Upscaling default"
    // combo, per 3.3 A's "until the user picks any item" rule. Read by both to show the one-time
    // amber note; never written back to disk (inferred fresh from the pref keys every load).
    bool averSrMigrationNoteArmed_ = false;
    // Set the first time updateAverSrAuto is about to apply a non-Off level THIS SESSION -- arms
    // display.renderScalePending (and renderScaleCookieArmed_'s 30-frame clear) first, the same
    // crash-cookie protection Manual/named-level LOAD already gets, extended to Auto mid-session.
    bool averSrArmedNonOffOnce_ = false;
    // Fires the "[AverSR] ... scene WxH -> present WxH" line exactly once per process (3.3 A's
    // mandatory startup log, required on every run including --frames) -- see updateAverSrAuto.
    bool averSrStartupLogged_ = false;
    // Why the CURRENTLY APPLIED level is what it is -- Auto/Manifest/User/Cli/ForcedOff
    // (Scalability.hpp's AverSrSource), read by the Project Settings line and the Display combo's
    // "Auto (<level> from <source>)" label. Written every frame by updateAverSrAuto, so this is never
    // stale by more than one frame. Guarded on VOXI too: with no ladder in the tree, there's no
    // source to name.
#if AVER_MODULE_VOXI
    voxi::AverSrSource averSrSource_ = voxi::AverSrSource::Auto;
#endif
    // Project's own AverSR default combo's live edit state (Project Settings > Rendering, page 1) --
    // mirrors project_.averSr like occlusionCullEnabled_ mirrors project_.occlusionCull, but
    // unconditionally on every project open: -1 (follow Overall preset) is itself a meaningful choice,
    // so a project that doesn't pin one must reset to -1, not inherit the PREVIOUS project's pin
    // (applyProjectRenderSettings' own "mirrors, not a delta" comment on projectBackend_/
    // frameBudgetMs_ describes the identical hazard). Captured back in captureRenderSettingsFromUi.
    int  averSrProjectDefault_ = -1;
    // Constructed lazily on the first non-Off quality; never rebuilt, only dropped to null on Off.
    // `factory` must outlive every execute() call (AverSrSpatial.hpp) -- satisfied by the same
    // rhi::IDevice::resources() the editor uses for the device's lifetime.
    std::unique_ptr<aver::sr::SpatialUpscaler> averSrUpscaler_;
    // --edge-aa: constructs a real aver::sr::FxaaResolve, same as averSrUpscaler_ for SpatialUpscaler.
    // OFF (default) never constructs one, keeping an unused build bit-identical. SHARES ONE
    // rhi::IDevice UPSCALER SLOT with AverSR (setUpscaler takes one pointer, not a list);
    // applyUpscalerSlot() decides which wins -- not a limitation this task's measurements hit, since
    // none of its four configurations use --aversr.
    bool edgeAaEnabled_ = false;
    std::unique_ptr<aver::sr::FxaaResolve> edgeAaUpscaler_;
    // Said once per session, not once per frame: updateAverSrAuto forces Auto to Off every frame
    // --edge-aa occupies the upscaler slot; logging that every frame would flood the log.
    bool edgeAaAverSrWarnLogged_ = false;

    // ---- frame interpolation (docs/rendering/NEURAFI.md) ----
    // Who decides, highest first: --frame-interp (frameInterpCli_, -1 = not given); during Play, the
    // project's RENDER.FRAMEINTERP; while editing, the Editor Preference below (off by default). The
    // generator is built on first use and installed on the device for the session.
    int  frameInterpCli_ = -1;
    bool frameInterpWhileEditing_ = false;   // Editor Preferences, display.frameInterpWhileEditing
    // The gather's path and in-engine training of the learned one (NEURAFI.md §3.5): Editor
    // Preferences display.frameInterpTrajectory / display.frameInterpTrain, for editing and Play alike (a
    // per-machine quality choice, like AverSR's Display setting). The CLI flags outrank them.
    int  frameInterpTrajectory_ = 2;         // neurafi::Trajectory (0 straight, 1 quadratic, 2 learned)
    bool frameInterpTrain_ = false;
    int  frameInterpTrajectoryCli_ = -1;     // --frame-interp-trajectory; -1 = not given
    bool frameInterpTrainCli_ = false;       // --frame-interp-train
    // The status bar's frame rate counts interpolated frames too (real + interpolated) or real frames
    // only; chosen by clicking it. display.fpsCountsInterpolated.
    bool fpsCountsInterpolated_ = true;
    std::unique_ptr<aver::neurafi::NeuraFI> frameInterpolator_;
    // Decides and pushes this frame's frame-interperation state; returns whether it is wanted (the
    // G-buffer must then be on).
    bool updateFrameInterpolation(aver::rhi::IDevice* dev);
#endif
    int  rdAblate_=0;                // --rd-ablate: AVER_RD_ABLATE for PSRayDriven, 0 = normal
    f32  rtDenoiseMotionTaper_=0.0f; // --rt-denoise-motion: 0 = no taper, the shipped default
    bool ptSceneViewYieldLogged_=false;   // say once, per mode change, that the PT view yielded
    int  refractionOverride_=-1;     // --refraction: -1 not given, else the mode
    f32  refractionStrengthOverride_=-1.0f;
    f32  refractionFadeOverride_=-1.0f;
    bool frameTimeReport_=false;     // --frame-time: report the frame period, to price the above
    bool msOverride_=false;          // --ms: force the mesh shader geometry path
    u32  probeX_=0, probeY_=0;       // --probe X Y: absolute capture pixel (0 = viewport centre)
    f32  probeU_=-1.0f, probeV_=-1.0f;   // --probe-rel U V: a FRACTION of the viewport rect
    bool camOverride_=false;         // --cam X Y Z PITCH YAW: aim the viewport camera outright
    f32  camWobbleDeg_=0.0f;         // --cam-wobble DEG PERIOD: yaw amplitude, 0 = no motion
    i32  camWobblePeriod_=0;         // ...and its period in FRAMES; sin is 0 at every multiple
    f32  camWobbleBaseYaw_=0.0f;     // the yaw to swing about, latched on the first wobbled frame
    bool camWobbleBased_=false;
    i32  camWobbleStopFrame_=0;      // --cam-wobble-stop N: wobble only while frame < N; 0 = never stop
    // --set NAME VALUE pairs, applied once through runConsoleLine on the first frame with a device.
    std::vector<std::pair<std::string, std::string>> consoleSets_;
    bool consoleSetsApplied_=false;
    f32  camTranslateSpeed_=0.0f;    // --cam-translate SPEED: forward-flight, cm/frame, 0 = no motion
    f32  camWanderAmp_=0.0f;         // --cam-wander AMP SPEED: non-repeating drift, 0 = off
    f32  camWanderSpeed_=1.0f;
    bool camWanderBased_=false;
    Vec3 camWanderBasePos_{};
    f32  camWanderBaseYaw_=0.0f, camWanderBasePitch_=0.0f;
    // --mesh-heap default|upload (W4): false (default) = static mesh vertex/index buffers on the
    // Upload heap, today's behaviour on every backend; true moves them to the Default heap (see
    // rhi::IDevice::setStaticMeshHeapDefault). Applies to createMesh calls made after it's set, so
    // it's read once, at the top of loadProjectMeshes, before the first mesh upload -- not reasserted
    // per frame, since meshes load once.
    bool meshHeapDefault_ = false;
    // --lod-share-vertices 0|1: coarser LODs share LOD0's vertex buffer (createMeshSharingVertices),
    // falling back to their own copy on refusal. 0 gives each level its own copy.
    bool lodShareVertices_ = true;

    Vec3 camPosOverride_{};
    f32  pitchOverride_=0.0f, yawOverride_=0.0f;   // radians, converted in setCamera
    bool useWarp_=false;             // --warp: run on the D3D12 software rasteriser
    // The engine, for the log sink to write the startup splash through. Set at the top of onInit and
    // left set: loadingScreenActive() gates the write and Engine clears that itself on close, so
    // there's one owner of "is it still up".
    Engine* engineForSplash_ = nullptr;
    std::thread::id mainThreadId_{};
    bool windowedOverride_=false;    // --windowed: kept so the flag still parses; windowed is the default now
    bool fullscreenOverride_=false;  // --fullscreen: opt an interactive run INTO borderless fullscreen
    std::string backendName_;   // --backend: which RHI backend to ask for first
    // RENDER.BACKEND as the OPEN PROJECT states it. Separate from backendName_ (what this run
    // launched with): editing the project's choice must not retarget a running device, and showing
    // the running backend beside the setting is only honest if the two are distinct values.
    std::string projectBackend_;
    std::string runningBackend_ = "?";   // what the device actually came back as, latched at init
    bool debugLayer_=false;          // --debug-layer: validate every graphics call (a real per-call tax)
    std::string scriptsDir_;         // --scripts <dir>: where to look for user script assemblies
    std::string spawnTestClass_;     // --spawn-test <ClassName>: headless actor-loop test trigger
    bool spawnTestDone_=false;       // the spawn is one-shot, done on the first frame scripts are ready
    int32_t spawnTestEntity_=0;      // the spawned test entity, destroyed a few frames later
    int spawnTestFrames_=0;          // frames since the test spawn, so the destroy is one-shot too
    // The open drawer's CURRENT animated height in pixels, 0 when closed. Written by drawDrawer, read
    // by the viewport hint so it sits above the drawer -- published rather than recomputed, to avoid drift.
    f32  drawerPixelH_=0.0f;
    bool playTest_=false;            // --play-test: headless begin_play -> tick -> end_play trigger
    bool playTestBegun_=false;       // begin_play has fired (one-shot, once a GameMode class is declared)
    int  playTestWait_=0;            // frames spent waiting for a GameMode class before giving up
    int  playTestFrames_=0;          // frames since begin_play, so the Stop is one-shot too
    editor::ProjectBrowser browser_;
    fmt::ProjectDesc project_;
    std::string projectPath_;        // <path>.ocproject given on the command line
    std::string openMapPath_;        // <path>.ocmap given on the command line, if any
    bool browserActive_=false;
    // Whether THIS launch's own command line was eligible to register as the single-instance
    // primary -- see setSingleInstanceEligible's own comment for the exact condition and why it is
    // computed in createApplication rather than here.
    bool singleInstanceEligible_ = false;
    // ONE PENDING OPEN, whatever asked for it: File > Open Level's picker, a Content Browser
    // double-click, or a path forwarded from a second launch -- only the last of those used to exist.
    // Funnelled through one request so the unsaved-changes guard and the class-placement spawn happen
    // once each, in one place, instead of once per caller with the third one forgetting.
    // DEFERRED, not immediate: cbOpenEntry (Content Browser) has no Engine& to hand loadLevel.
    // Latched here, drained in onUpdate, like the forwarded-open poll beside it.
    std::string pendingOpenPath_;
    std::string pendingOpenWhy_;      // how it was asked for, for the modal's own sentence
    // The modal shown when a pending open would discard unsaved work. Separate from exitPrompt_:
    // one modal meaning both risked "Discard and exit" when what's about to happen is opening a level.
    bool        pendingOpenPrompt_ = false;
    bool        pendingNewLevel_ = false;   // File > New Level, waiting on the unsaved-changes prompt
    // File > Launch in Aver Engine Runtime, waiting on drawLaunchRuntimePrompt's unsaved-changes ask.
    bool        launchRuntimePrompt_ = false;
    // File > Open Level's own picker: the list is rebuilt when it opens, not per frame.
    bool        openLevelPicker_ = false;
    bool        armOpenLevelPicker_ = false;   // --open-level-picker, consumed on the first draw
    std::string openLevelByName_;              // --open-level, consumed on the first draw
    std::vector<editor::LevelEntry> openLevelList_;
    int         openLevelSelected_ = -1;
    std::string openLevelError_;
    rhi::TextureHandle logoTexture_=0;   // 0 when logo.png was absent or undecodable
    rhi::TextureHandle fileIconsTexture_=0;     // the Content Browser file-type sprite sheet (4 tiles)
    u64 fileIconsUiId_=0;
    f32 fileIconAspect_=0.74f;                  // measured from the sheet; the literal is only the fallback
    rhi::TextureHandle folderIconsTexture_=0;   // the folder sheet (2 tiles: plain, module)
    u64 folderIconsUiId_=0;
    f32 folderIconAspect_=1.24f;
    rhi::TextureHandle assetIconsTexture_=0;    // the generated asset sheet (anim, skeleton, mesh)
    u64 assetIconsUiId_=0;
    f32 assetIconAspect_=0.74f;
    // The tile counts, named once each -- used to be two unconnected literal 4s (load call, blit),
    // so changing either alone sampled the wrong UV window and every icon silently shifted.
    static constexpr int kFileIconTiles   = 4;
    static constexpr int kFolderIconTiles = 2;
    static constexpr int kAssetIconTiles  = 4;   // anim, skeleton, mesh, graph
    // Tiles are packed into one int so a cached DirListing entry stays one field: below the base is
    // the file sheet, at or above it the asset sheet.
    static constexpr int kAssetTileBase   = 100;
    std::unordered_map<std::string, std::pair<std::filesystem::file_time_type, int>> fileIconCache_;
    // A .ocgraph's DOMAIN record, read once and kept against the file's mtime -- the SAME cache
    // shape as fileIconCache_ above, for the same reason: dirListing() rebuilds every 20 frames, and
    // this is where that rebuild reads it, not the per-frame draw.
    std::unordered_map<std::string, std::pair<std::filesystem::file_time_type, editor::GraphAssetFamily>>
        graphDomainCache_;
    std::unordered_map<std::string, DirListing> dirCache_;
    int frameNo_ = 0;                                      // bumped once per UI frame; the cache freshness clock
    rhi::TextureHandle compileIconTexture_=0;   // the Compile C# status sprite sheet (3 tiles)
    u64 compileIconUiId_=0;
    u64 logoUiId_=0;
    f32 logoAspect_=1.0f;
    editor::ToolsMenu tools_;
    // Gizmo coordinate space. Honoured by drawGizmo, pickAxis, applyMove and applyRotate; the SCALE
    // tool ignores it and is always local, because o.scale is per-object-axis by definition.
    bool worldSpace_=true;
    bool giDebugView_=false; Vec3 giCenter_{0,0,300}; f32 giExtent_=1200.0f;   // cm
    bool giConeTraceOff_=false;   // --no-gi-cone: see setGiConeTraceOff's own comment
    // --view-mode undenoised / the dropdown's independent "Undenoised" toggle: forces a bundle of
    // existing runtime knobs off on the per-frame Voxi scratch copy (see onUpdate's UNDENOISED
    // comment for the list). NOT PERSISTED, same reason as giDebugView_: reasserted every frame.
    bool undenoised_=false;
    // True when --view-mode or --unlit set the view for this run: stored viewport.wireframe/
    // viewport.unlit neither override it on load nor get overwritten by it on save
    // (SandboxSettings.cpp). Without it, the stored Lit silently replaced --view-mode wireframe.
    bool viewModeFromCli_=false;
#if AVER_MODULE_VOXI
    voxi::VoxiRenderer voxiRenderer_;
    bool voxiAttached_=false;
    // Viewport's ray-hit/triangles debug view (Ray Hit: Instances/Materials/Distance, Triangles), or
    // None (dropdown "Debug" section / --view-mode rayhit-*|triangles). Type IS
    // voxi::VoxiRenderer::ViewDebug, so it sits inside the module guard unlike undenoised_ above. NOT
    // PERSISTED: reasserted onto voxiRenderer_ every frame from onUpdate (beside setUnlit's call
    // site), not saved to a project or editor.ini.
    voxi::VoxiRenderer::ViewDebug debugView_ = voxi::VoxiRenderer::ViewDebug::None;
    // Last frame's EFFECTIVE voxi::Settings::rtRenderMode, after onUpdate's auto-switch (wireframe_/
    // G-buffer debug forcing raster, ray-hit/triangles debug forcing ray-driven) -- not the authored
    // project value. Lets onUpdate tell an actual mode transition from an ordinary frame, so history
    // resets only then. -1 = no frame run yet, never equal to a real rtRenderMode (0 or 1), so the
    // first frame gets exactly one harmless reset.
    i32 lastEffectiveRtRenderMode_ = -1;
#endif
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // Registration is non-owning, same contract as voxiRenderer_ just above: particleRenderer_ must
    // outlive the device, torn down in onShutdown.
    particles::ParticleRenderer particleRenderer_;
    bool particlesAttached_=false;
#endif

    // Water is an INDEPENDENT module from Particles (see the include-block comment near the top of
    // this file) -- b6881c49 previously nested this group inside `AVER_MODULE_PARTICLES &&
    // AVER_MODULE_SCENE`, so a particles-off, fluids-on tree declared none of these members while
    // setWater() (correctly guarded on AVER_MODULE_FLUIDS alone) still used them -- undeclared-
    // identifier errors, not missing-header ones, which is why this half survived fixing only the
    // includes.
    // OFF UNLESS ASKED FOR, unlike particles above: a particle renderer with no emitters draws
    // nothing, whereas a water plane is an infinite sheet that would appear in every level ever
    // opened. --water <heightCm> is the opt-in.
#if AVER_MODULE_FLUIDS
    // The level's analytic surface and simulated volumes, shared with the runtime.
    game::GameWater water_;
#endif
    // These two stay OUTSIDE the guard: the flag is parsed either way, so a build without the
    // module can say "this build has no water" rather than silently ignoring --water.
    bool  waterEnabled_  = false;
    f32   waterHeightCm_ = 0.0f;
#if AVER_MODULE_SCRIPTING
    static i32 animCurve(i32 entity, i64 nameHash, f32* outValue, void*);

#if AVER_MODULE_SYNAPSE_SCENE
    static i32 synapseTarget(i32 entity, f32* outX, f32* outY, f32* outZ, void*);

#if AVER_MODULE_FRAMEWORK
    static scene::Entity synapseTargetResolver(void*);
#endif

    static i32 synapsePerception(i32 entity, i32* outCanSee, i32* outLastTarget,
                                 f32* outTimeSinceSeen, void*);
#endif

    // NEEDS THE SCENE AS WELL AS SCRIPTING, but said only the latter: parameter is a scene::Entity,
    // sink is aver::anim::AnimNotifyFn (Aver.Anim.Scene, added only `if(AVER_MODULE_SCENE)`), so a
    // SCENE=OFF tree has neither the type nor the header -- Scripting stays on in that tree
    // (module-matrix.ps1's scene-off row turns off SCENE and FRAMEWORK only), which made the gap
    // visible.
#if AVER_MODULE_SCENE
    static void animNotify(scene::Entity e, const char* name, void* user);
#endif

    scripting::ScriptHost scripts_;
#endif
    // The retained game UI's renderer. Heap-owned because create() may decline.
    aver::render::ui::UiRenderer* gameUi_ = nullptr;
    bool skinTest_ = false;       // --skin-test: GPU skinning against its CPU reference, then exit
    std::unique_ptr<aver::render::SkinSelfTest> skinSelfTest_;
    // VERDICTS LATCHED OUT OF THE TWO TESTS ABOVE, because onShutdown destroys them before
    // exitCode() is read. -1 = never asked for, 0 = passed, 1 = failed, 2 = never reached a verdict.
    int skinSelfTestExit_ = -1;
    int skinDrawExit_     = -1;
    bool skinDrawTest_ = false;   // --skin-draw-test: does the RASTERISER read the skinned buffer
    // BESIDE skinDraw_, outside the scene guard (used to sit one line inside): ThumbnailCache
    // (sandbox/src/ThumbnailCache.hpp) stands on the RHI and ActorPreview only, never names a
    // scene::Entity. The content browser already treats that as settled -- its texture-thumbnail
    // branch calls requestTexture()/textureIdForPath() with a standing comment saying so.
    // Content browser's rendered mesh thumbnails: a small ActorPreview of its own, not shared with
    // skinnedScene_ or the asset editors' preview (either would fight it for its draw list, or force
    // every thumbnail to the wrong size). A value member like particleRenderer_: init() can fail, but
    // nothing needs the object to not exist, only to be inert.
    aver::editor::ThumbnailCache thumbnails_;
    std::unique_ptr<aver::editor::SkinDrawTest> skinDraw_;
#if AVER_MODULE_SCENE
    // The scene join: gives every entity with a CSkeletalMesh its own posed mesh. Null when the
    // skinning shader would not compile, in which case skinned entities simply draw at rest.
    std::unique_ptr<aver::render::SkinnedScene> skinnedScene_;
#if AVER_MODULE_RENDER_SOFTBODY
    // Beside skinnedScene_ because it is the same kind of thing: a per-entity vertex source that the
    // scene pass substitutes for the authored mesh. The two never both claim one entity -- see the
    // draw site, where skinning is asked first and soft body only fills in where it declined.
    std::unique_ptr<aver::render::SoftBodyScene> softBodyScene_;
#endif
#endif
    std::string skinSceneDir_;
    std::string shaderSourceDir_;                 // --shader-source <dir>, empty = off
    aver::DirectoryWatcher shaderWatch_;    // --skin-scene-test <dir>: where the cooked rig lives
    std::unique_ptr<aver::editor::SkinSceneTest> skinScene_;
    bool particleTest_ = false;   // --particle-test: a dust cloud straddling an opaque occluder, so
                                   // the transparent pass's own depth test shows in one screenshot
    bool noParticleGi_ = false;   // --no-particle-gi: see setNoParticleGi's own comment
    // VERIFICATION-ONLY INSTRUMENTATION (not part of the particles slices): --particle-stress <N> <M>
    // spawns N grid-arranged dust-style emitters, each capped at M particles, purely to price the
    // system for the parity-and-price pass (see setParticleStress).
    int  particleStressEmitters_ = 0;
    int  particleStressMaxParticles_ = 0;
    bool particleStressSecondEmitter_ = false;   // --particle-stress2: diag, see its own comment
    // Same purpose: direct CPU wall-clock timing around particles::particleSystem().tick(), since no
    // --frame-time/GPU-marker path measures CPU simulation separately from the GPU draw. Accumulated
    // every frame, printed once at shutdown; steady_clock costs single-digit ns, always-on to prove it
    // didn't skew any earlier probes.
    f64  particleTickAccumSec_ = 0.0;
    u64  particleTickFrames_ = 0;
    bool reflTest_ = false;       // --refl-test: are ray-traced reflections global?
    bool furnaceTest_ = false;    // --furnace-test: does the shading model conserve energy?
    bool furnaceGrid_ = false;    // --furnace-grid: the same question across roughness/metallic
    f32  furnaceTilt_ = 0.0f;     // --furnace-tilt DEG: read that grid at grazing incidence
    bool furnaceSun_ = false;     // --furnace-sun: the variant where only the DIRECT term is lit
    f32  sunAngle_ = -1.0f;       // --sun-angle: negative leaves the sky's own value alone
    bool ptFurnaceTest_ = false;  // --pt-furnace: the same question asked of the path tracer
    std::unique_ptr<aver::pt::PtFurnaceTest> ptFurnace_;
    // --pt-scene: the path tracer pointed at the real scene. ptSceneViewWantEnabled_ below is the
    // one flag that matters now (see its own comment) -- a separate ptSceneViewFlag_ here too went
    // unread once syncPtSceneView() took over registration, and was removed.
    std::unique_ptr<aver::pt::PtSceneView> ptSceneView_;
    // REQUESTED state (--pt-scene at startup, or the settings-page Quality combo later);
    // ptSceneView_ != nullptr is the ACTUAL one. syncPtSceneView() reconciles the two, only from
    // onUpdate() before beginFrame() -- deliberately not a read of voxi::Settings::pathTracing, so
    // this flag and PT's registration keep working with AVER_MODULE_VOXI off.
    bool ptSceneViewWantEnabled_ = false;
#if AVER_MODULE_VOXI
    // N8: the Path Tracing tier onUpdate's reconcile block last saw vx.settings().pathTracing read
    // as. Guarded on AVER_MODULE_VOXI, unlike ptSceneViewWantEnabled_: its TYPE is voxi::Quality,
    // which can't exist without Voxi -- the reconcile block that reads it is guarded the same way.
    voxi::Quality ptTierSeen_ = voxi::Quality::Off;
#endif
    // --pt-scene WAS GIVEN ON THE COMMAND LINE. Sticky for the session, separate from the want flag
    // above (written by CLI, settings combo, toggle-test flags, AND a project manifest): answers
    // which ONE asked -- see applyProjectRenderSettings, where a manifest used to silently outrank a flag.
    bool ptSceneViewFromCli_ = false;
    bool ptSceneViewUnavailable_ = false;   // init() refused once this session -- stop re-asking
    // A1: true while syncPtSceneView() holds ptSceneViewWantEnabled_ down because ray-driven primary
    // visibility is painting the scene -- set/cleared in the same block that decides it (see
    // ptSceneViewYieldLogged_, which only tracks whether the log line fired once). Read by the Path
    // Tracing page's Quality-combo tag (PtRenderConflict.hpp's choosePtViewTag) so the UI says why.
    bool ptSceneViewSuppressedByRayDriven_ = false;
    // Wireframe/a G-buffer debug view withdrew the path-traced view's want (syncPtSceneView's
    // "A RASTER-ONLY VIEW MODE HAS THE FRAME" block) -- remembered so leaving the view mode gives
    // it back, and only then.
    bool ptSceneViewSuppressedByViewMode_ = false;
    // --pt-scene-toggle-on/--pt-scene-toggle-off [N]: VERIFICATION ONLY. Simulates flipping the
    // Path Tracing Quality combo N frames into a bounded run, proving the RUNTIME toggle (not just
    // --pt-scene's register-before-frame-1 path). Two independent countdowns from process start (not
    // "N after ON"), so the caller picks values (e.g. on=5, off=15) freely.
    // --pt-quality-ramp [N]: 0 is off. See the ramp in onUpdate.
    int ptQualityRampEvery_ = 0;
    int ptQualityRampCountdown_ = 0;
    int ptSceneToggleOnAutoFrames_ = 0;
    int ptSceneToggleOffAutoFrames_ = 0;
    // --sun-set-at N ELEV AZIM and --gi-history-reset-at N: VERIFICATION ONLY. Simulates dragging the
    // Directional Light panel's Elevation/Azimuth sliders + resetgihistory/resetdenoiserhistory, N frames
    // into a --frames run -- the only way to capture indirect light AFTER a live sun move (a level's
    // SUN line, applied before frame 1, can't show this). Countdowns from process start, the same
    // shape as the two above.
    int sunSetAtFrames_ = 0;
    f32 sunSetElevDeg_ = 0.0f, sunSetAzimDeg_ = 0.0f;
    // --sun-sweep START DEG: the DRAG, not the jump -- counts down like sunSetAtFrames_, then turns
    // the sun's azimuth by sunSweepDeg_ every frame for the rest of the run, so a capture always lands mid-drag.
    int sunSweepFrames_ = 0;
    f32 sunSweepDeg_ = 0.0f;
    // --sun-sweep-frames N: the drag LETS GO after N turns (0 = never), and the final angles are
    // logged -- so a capture k frames later measures how long lighting takes to catch up with a sun
    // that has stopped, against a --sun-set-at run to those same angles.
    int sunSweepTurnsLeft_ = 0;
    int giHistoryResetAtFrames_ = 0;
    // True while a stored non-unity render scale is on trial this session; see the prefs-apply site.
    bool renderScaleCookieArmed_ = false;
#if AVER_MODULE_SR
    // --aversr-cycle [N]: drive the on-then-off transition that used to free the upscaler out from
    // under the device. Verification-only; 0 means never.
    int averSrCycleFrames_ = 0;
#endif
    std::unique_ptr<aver::editor::ReflTest> refl_;
    int  reflBeaconIndex_ = -1;   // which objects_ entry the schedule shows and hides
    rhi::MeshHandle unitCubeMesh_ = 0;   // the editor's own unit cube, half-extent 1
    bool showUiDemo_ = false;
    std::string matSaveStatus_;   // what the last 'Save to C#' did
    unsigned centralDock_ = 0;    // the dock node an opened asset editor lands in
    bool levelFocused_ = true;    // the Level tab holds the keyboard
    // The two panels a selection is also made from, so the edit verbs reach a selection made
    // there. False when their window is closed, which is correct: a hidden panel holds no focus.
    bool outlinerFocused_ = false;
    // A row asked to be deleted; answered after the tree walk (drawOutlinerRow). GUARDED like
    // firstPersonPawn_: type is scene::Entity, landed here by proximity to the Outliner's other
    // state. Every use is already inside `AVER_WITH_IMGUI && AVER_MODULE_SCENE` (SandboxPanels.cpp)
    // -- SCENE alone here, since the type is the only thing forcing a guard; a no-UI tree carrying
    // four unread bytes isn't worth a second condition to keep in step.
#if AVER_MODULE_SCENE
    scene::Entity outlinerDeleteRequest_ = scene::kInvalidEntity;
#endif
    bool detailsFocused_  = false;
    bool levelHovered_ = true;    // the cursor is over the Level tab and it is topmost there
    bool inputProbe_ = false;
    bool levelVisible_ = true;    // the Level tab is the selected tab
    bool openLegacy_ = false;   // --open-legacy
    std::string openAsset_;
    std::string selectEntity_;   // --select
    std::string lastOpenAssetPath_;  // last path --open-asset opened; openAsset_ itself is cleared
                                      // once consumed, so --graph-select needs its own copy to find it
    std::string graphSelectNode_;
    std::string graphTab_; // --graph-select <nodeId>; see setGraphSelectNode's own comment

    editor::ProjectUpgrade pendingUpgrade_;
    bool        upgradeAsked_ = false;
    bool        exitPrompt_ = false;      // the unsaved-changes modal is up
    std::string exitPromptError_;         // why a "Save all" attempt failed
    // Every PCGVOLUME the loaded level carried, kept verbatim so a save cannot drop them. The editor's
    // own copy, seeded from the load: the Project Settings PCG page edits it in place.
    std::vector<fmt::OcPcgVolume> levelPcgVolumes_;
    // The loaded level's own header, placements and PCG volumes stripped. SAME REASONING as
    // levelPcgVolumes_, generalised: saveLevel used to build a fresh OcWorldData from the editor's
    // state, resetting every unmodelled field to default (SPAWN deleted, BUILD to 0, ID recomputed,
    // `lux` reverted to 100000). Starting from what the file said, overwriting only what the editor
    // owns, fixes all of those plus any field added to the format later.
    // ---- the 3D-viewport icon renderer, and the Player Start marker it draws ----
    // viewportIconsReady_ is the ONE flag the render walk consults to skip the Player Start's cube,
    // false unless the feature AND its texture both came up -- every failure path draws the cube as before.
    // Not behind AVER_FLUIDS_SIMULATED: nothing here is a fluid (ViewportIconRenderer is sandbox's
    // own header, unconditional; the marker is the Player Start). The guard was proximity -- it
    // wrapped these three and nothing else -- and AVER_FLUIDS_SIMULATED is defined only when PHYSICS
    // is in the tree (modules/fluids/CMakeLists.txt), so -DAVER_MODULE_PHYSICS=OFF used to delete
    // these members while every use site (guarded on SCENE, where playerStart_ lives) stayed compiled
    // in.
    editor::ViewportIconRenderer viewportIcons_;
    bool viewportIconsReady_ = false;
    editor::ViewportIconRenderer::IconHandle playerStartIcon_ = editor::ViewportIconRenderer::kNoIcon;
    // The capsule/arrow line meshes, built once like the gizmo handles (gzMove_ etc.) and placed with
    // a world matrix per frame. Independent of viewportIconsReady_ above -- that flag gates only the
    // SPRITE (it needs the icon renderer and its texture); these are ordinary line meshes, drawn
    // through IDevice::drawLines like every other piece of chrome, so they never fall back to the
    // marker's pick cube the way the sprite does.
    rhi::LineHandle playerStartCapsule_ = 0, playerStartCapsuleSel_ = 0, playerStartArrow_ = 0;
    fmt::OcWorldData levelHeader_;
#if AVER_MODULE_PBR
    // pinMaterialResident()/unpinMaterialResident()'s own storage -- see their comment for the
    // contract. Checked by bindMaterialsForLevel()/unloadLevel() so a level-scoped release never
    // destroys a material an open tab or the current selection still holds.
    std::unordered_set<std::string> pinnedMaterialNames_;
#endif
    // Whether each level entity's placement said `nocollide`. No component for this: it's a
    // load-time instruction that nothing on the entity records afterwards, so without it the save
    // forced `collide = true` on everything and `nocollide` never survived a round trip.
    std::unordered_map<u32, bool> entityCollide_;
    // Each entity's authored object animation; absent = none. The CAnimator on the entity is only the
    // live clock, so saveLevel reads this map, never the component -- Play must not leak into a save.
    std::unordered_map<u32, EntityAnim> entityAnim_;
    // The Details panel's Animation drag in flight: every selected entity's EntityAnim from the frame
    // the widget was grabbed, so the whole drag is one undo entry (see the Animation section there).
    std::vector<EditCmd::AnimChange> animEditBefore_;
    // Entities placed with `snap`, and the AUTHORED z offset each was placed at. Absent = not snapped.
    std::unordered_map<u32, f64> entitySnapZ_;
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS
    // driveAnimatedBodies' list, kept between frames and rebuilt only when it can have changed: the
    // three counts below are what it was built from, and the flag is dropped whenever Play is not
    // driving (editing, paused, stopped) or physics reports a listed body gone.
    std::vector<world::AnimatedBody> animatedBodies_;
    bool animatedBodiesBuilt_ = false;
    usize animatedBodiesFromAnim_ = 0;
    usize animatedBodiesFromBodies_ = 0;
    u64 animatedBodiesFromRev_ = 0;
#endif

    // True while the open level loaded through the legacy OCMAP path, not the ordinary OCWORLD one --
    // decided by which RECORDS the file uses, not its header/extension. saveLevel reads it to choose
    // the writer: OCMAP must save as OCMAP, or ROOT/CLIENT/SURFACE/GROUND/KILLZ/DEFORM survive the
    // read only to be dropped on write. Reset by unloadLevel, so a later .ocworld isn't mistaken for one.
    bool levelIsLegacyOcmap_ = false;
    // The loaded level's own legacy header, placements stripped -- levelHeader_'s pattern, for the
    // five record kinds only OCMAP has and this editor has no UI for (ROOT, CLIENT, SURFACE, GROUND,
    // KILLZ). saveLevelAsOcmap overwrites only NAME/ID/BUILD/ALGO/SPAWN + placements, so unshowable
    // fields survive every save.
    fmt::OcMapData legacyMapHeader_;
    // Which record kind each legacy-loaded entity came from (true = DEFORM, false/absent = PLACE),
    // plus the field OcWorldPlacement has no room for: a PLACE's numeric SURFACE-table index
    // (entityLegacySurface_; -1 = "the asset's own") or a DEFORM's soft-body material name
    // (entityLegacyMaterial_, e.g. "rubber") -- see onLegacyOcmapInstantiated's "MATERIAL IS
    // DELIBERATELY LEFT EMPTY" comment for why neither reaches CMeshRenderer. An entity absent from
    // entityLegacyDeform_ saves as an ordinary PLACE with surface -1.
    std::unordered_map<u32, bool> entityLegacyDeform_;
    std::unordered_map<u32, i32> entityLegacySurface_;
    std::unordered_map<u32, std::string> entityLegacyMaterial_;
    f32  uiDemoHealth_ = 0.72f, uiDemoStamina_ = 0.44f, uiDemoScroll_ = 0.0f, uiDemoClock_ = 0.0f;
    // The game UI's font. Invalid until loadGameUiFont finds one beside the exe; addText on an
    // invalid font draws nothing, which is what makes a missing font non-fatal.
    ui::UiFont uiFont_;
    rhi::TextureHandle uiFontTexture_ = 0;

    rhi::MeshHandle cubeMesh_=0; u32 cubeTris_=0; int spawnCount_=0;
    // The project's content index, asset resolvers, mesh registry, built-in meshes and surface looks,
    // and material cache: the runtime's GameContent, shared with AverEngineRuntime.exe.
    game::GameContent content_;
    // The loaded level: parsing, placement instantiation, environment, spawn record, PCG volumes, class
    // placements and bounds -- the runtime's GameLevel. The editor's live entity list (levelEntities_,
    // which edits add to and remove from) and every per-entity record stay SandboxApp's.
    game::GameLevel level_;
#if AVER_MODULE_PBR
    // mesh id -> its cached outline line mesh (0 = this mesh yields no outline). See
    // selectionOutlineLines; dropped with the project's meshes.
    std::unordered_map<u64, rhi::LineHandle> selOutlineLines_;
#endif
    // The ASSET id of the selected mesh (what meshPathById_ and the outline cache key on), distinct
    // from selectionMesh_ (a GPU upload handle, not a file). UNGUARDED although the outline cache
    // above it is not: this is a u64 that the placeholder Floor/Cube loop clears in every
    // configuration -- what a selection IS, not what PBR does with one. The cache stays behind PBR
    // because the outline it holds is built from the material system's own geometry.
    u64 selectionMeshId_ = 0;

    // ---- landscape (opt-in; --landscape <path>, or <levelname>.ocland beside the level) ----
    // Hosts ONE open .ocland section via game::GameLandscape below: render, collision, level-
    // reference AND sculpt (missing: an asset-editor tab, LANDSCAPE_EDITOR.md slice 0). Independent
    // of AVER_MODULE_SCENE -- a section is not an ECS entity.
    // UNGUARDED, matching chunkStreamAutoFrames_ below: setLandscapePath() must compile with the
    // module off (command-line parsing is unconditional), so the field must exist unconditionally too.
    std::string landscapeCliOverride_;
#if AVER_MODULE_LANDSCAPE
    // The level's terrain section and ring -- render, collision and authoring -- shared with the runtime.
    game::GameLandscape landscape_;

    // Sculpt tool state. Radius/strength are shared across all four brush modes -- the same "one
    // knob set, the mode picks what it means" shape the transform tools' snap popups already use.
    f32 sculptRadiusCm_ = 500.0f;
    f32 sculptStrengthCm_ = 150.0f;
    bool sculpting_ = false;              // LMB down, over the terrain, with a sculpt tool active
    f32 sculptFlattenTargetCm_ = 0.0f;    // captured once per stroke -- see BrushParams::flattenTargetCm
    f32 sculptRampStartCm_[2] = {0.0f, 0.0f};   // captured once per stroke -- see BrushParams::rampStartCm
    f32 sculptRampStartHeightCm_ = 0.0f;        // captured once per stroke -- see BrushParams::rampStartCm
    u32 sculptNoiseSeed_ = 0;                   // captured once per stroke -- see BrushParams::noiseSeed
    bool sculptCursorValid_ = false;      // true when this frame's cursor ray actually hit the section
    Vec3 sculptCursor_{0, 0, 0};          // world hit point, for the brush-radius ring and the next tick
    rhi::LineHandle brushRing_ = 0;       // a unit ring in the XY plane -- landscape heights run +Z
#endif

#if AVER_MODULE_SCENE
    // Turns chunk streaming on/off around the editor camera. OPT-IN: nothing in modules/world's
    // generator or region files is touched until Window > Chunk Streaming, so an ordinary project
    // opens exactly as it always did.

    static f32 fogDensityForOpacityAt(f32 distanceCm, f32 targetOpacity);

    // GUARDED: scene::Entity doesn't exist with AVER_MODULE_SCENE=OFF; its only caller is already
    // inside a SCENE guard -- missing it broke module-matrix.ps1's scene-off row, the only thing that
    // checks this.
#if AVER_MODULE_SCENE
    bool anyChunkWorldOwns(scene::Entity e) const;
#endif

    void setChunkStreamingEnabled(bool on);

    void setDroneEnabled(bool on);

    u64 residentTriangleCount() const;

    // Always-on-while-streaming readout of StreamStats. pendingLoads/failedLoads are singled out:
    // they tell "working" (draining, zero failures) from "not keeping up" (pendingLoads staying high)
    // or "broken" (failedLoads growing).
#if AVER_WITH_IMGUI
    void buildChunkStreamingPanel();
#endif  // AVER_WITH_IMGUI

    static bool onOpenRequestThunk(void* user, const char* path);

    bool handleOpenRequest(const char* path);

    // Loads through level_ (GameLevel, shared with the runtime), which parses the file -- OCWORLD or
    // legacy .ocmap -- and instantiates its placements; the editor's own per-entity records are built
    // in the afterInstantiate hook this installs.
    void loadLevel(Engine& eng, const std::string& path);

    // loadLevel's afterInstantiate hook, one per file kind: the editor's labels, body map, save
    // bookkeeping, PlayerStart marker, camera and GI fit for what level_ just built.
    void onLevelInstantiated(const game::GameLevel::LoadedLevel& loaded);
    void onLegacyOcmapInstantiated(const game::GameLevel::LoadedLevel& loaded);

#if AVER_MODULE_FRAMEWORK
    void spawnClassPlacements();
#endif

    void applyLevelSky(const fmt::OcWorldData& w);

#if AVER_MODULE_VOXI
    // Fits the GI volume to the loaded level's bounds (level_.placementBounds).
    void fitGiVolumeToLevel();
#endif

    // Frames the editor camera on the loaded level's bounds (level_.placementBounds).
    void frameCameraOnLevel();

    // WHERE THE CAMERA WAS WHEN THE LEVEL WAS LEFT (LevelViewStore.hpp), per level, per user, in
    // <project>/Saved/LevelViews.ini. storeLevelView records the editor view under levelPath_ on
    // every way out of a level -- another level, New Level, closing the editor -- saved or not;
    // lookupLevelView finds `levelPath`'s entry, and it outranks the level's CAMERA record, which
    // is only as new as the last save. Both do nothing in a bounded (--frames) run, so captures and
    // gates still open at the level's own record.
    void storeLevelView();
    bool lookupLevelView(const std::string& levelPath, editor::LevelView& out) const;
    // Moves the camera to `v`, with the clamps the mouse-look and wheel paths enforce.
    void applyLevelView(const editor::LevelView& v);
    std::string levelViewsPath() const;
    // The editor view at the moment Play started: while a session drives the camera from the pawn,
    // that, not camPos_, is where the user left the EDITOR camera.
    bool preplayViewValid_ = false;
    editor::LevelView preplayView_{};

    bool requestOpenLevel(const std::string& path, const char* why);

    void applyPendingOpen(Engine& eng);

    void startNewLevel(Engine& eng);

    void openLevelDirect(Engine& eng, const std::string& path);

    void loadStartMap(Engine& eng);

    void unloadLevel(Engine& eng);

    bool saveLevel(const std::string& path);

    bool saveLevelAsOcmap(const std::string& path);

    // ---- WHAT PLAY IS ALLOWED TO CHANGE, AND WHAT STOP PUTS BACK ----
    // Play used to be a one-way door: aver_fw_end_play() left the world exactly as gameplay left it,
    // so Stop returned you to a level no longer the one you'd opened, recoverable only by reloading
    // and losing your edits. A TRANSFORM SNAPSHOT, not a reload: reloading would also throw away
    // unsaved edits made before Play. TRANSFORMS, VISIBILITY AND SPAWNED ENTITIES, deliberately, not
    // a full component snapshot: the honest subset of what physics/gameplay actually change.
    // Visibility joined once it became AUTHORED, saved data (a graph can call Entity.SetVisible
    // during Play, the same kMeshRendererVisible bit the Details panel saves) -- without this, Stop
    // left that toggle standing: hide a prop from a trigger, press Stop, and the saved state came
    // back changed by the last session.
    // True while Play runs on the ENGINE's default GameMode because the project declared none.
    bool defaultPawnPlay_ = false;
    struct PlaySavedTransform { scene::Entity e; Transform xf; bool visible = true; };
    std::vector<PlaySavedTransform> playWorldSnapshot_;
    bool playWorldCaptured_ = false;
    // What moved during this session, so Voxi keeps it out of the GI bake (PlayMobility.hpp).
    // Begun in capturePlayWorld, before anything spawns; ended in stopPlay.
    game::PlayMobility playMobility_;
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS
    // The level's `vehicle` placements as physics cars while Play runs (world::VehicleSystem), built
    // from level_'s vehicle placements in capturePlayWorld -- after the transform snapshot, like a
    // CRigidBody, so editing never has one settling on its suspension -- driven around
    // tickGameplayGroups in onUpdate, and ended in stopPlay before restorePlayWorld puts the
    // placements back.
    world::VehicleSystem vehicles_;
#endif

    std::vector<scene::Entity> levelEntities_;
    bool hasLevelSun_ = false;
    bool hasLevelSky_ = false;
    bool hasLevelFog_ = false;
    // "This level said something about clouds", NOT "this level has clouds" (fmt::OcWorldEnv::hasClouds).
    // Without the distinction, an overcast level switched to clear would write no record and come back overcast.
    bool hasLevelClouds_ = false;

#if AVER_MODULE_FRAMEWORK
    // GRAPH-AS-CLASS / any other class placement: level_.classPlacements() holds what the load
    // found; spawnClassPlacements spawns them and fills levelClassInstances_.
    // WHICH PLACEMENT EACH LIVE INSTANCE CAME FROM, carried explicitly, not by position: used to be
    // a bare vector<int32_t> read index-parallel -- wrong, since spawnClassPlacements `continue`s
    // past an unresolvable class (a warning, not an error) without pushing, so any undeclared class
    // desynced every later pairing.
    using ClassInstance = editor::LevelClassInstance;
    std::vector<ClassInstance> levelClassInstances_;
#endif

    // ---- chunk streaming (opt-in, Window > Chunk Streaming) ----
    // PCG chunk streaming around the editor camera and the drone, shared with the runtime; its
    // entities are transient, never saved or undo-tracked.
    game::GameStreaming streaming_;
#if AVER_MODULE_PHYSICS
    std::vector<int32_t> levelBodies_;
#endif

    // ---- graph-driven drone (opt-in, Window > Drone or --drone) ----
    // Proves a native scene can be driven by an .ocgraph end to end: spawned by setDroneEnabled(true),
    // ticked via scripts_.graphTick(), released by (false). Disjoint from levelEntities_ for the same
    // reason streaming_'s entities are: transient, never saved, never undo-tracked.
    scene::Entity droneEntity_ = scene::kInvalidEntity;
    // Whether PLAY started this drone, vs the user switching it on from Window > Drone. Stop takes
    // down only the former: ending play shouldn't remove something the user started themselves,
    // and without this flag the two can't be told apart.
    bool droneStartedByPlay_ = false;
    // Which .ocgraph drives the drone, relative to the project's Content directory. EMPTY by
    // default and set only by --drone-graph: the engine must not assume a project contains a file
    // with any particular name.
    std::string droneGraphRel_;
    bool droneGraphLoaded_ = false;
    f32  droneTimeSeconds_ = 0.0f;
    Vec3 dronePos_{};
    Vec3 droneVel_{};
    Vec3 droneLastPos_{};
    bool droneHaveLastPos_ = false;         // false right after enabling, so the first velocity
                                             // sample is zero rather than a spike from (0,0,0).
    u32  droneLogsLeft_ = 30;               // more than chunk streaming's 8: the ask is proving
                                             // MOVEMENT across several frames, not just one event.
#endif // AVER_MODULE_SCENE

    // Mouse capture, for a playing game: the cursor is hidden, confined and re-centred every frame.
    game::MouseCapture mouse_;
    Window* window_ = nullptr;   // borrowed from the engine in onInit, for the HWND

    // ---- THE EDITOR'S OWN INPUT ACCUMULATOR ----
    // Until this existed, ImGuiIO was the SOLE source of truth for keyboard/mouse in the editor
    // (SandboxApp never called Window::setEventCallback), so the tested aver::InputState accumulator
    // was wired up only by GameApp, a library with no executable. Doesn't compete with ImGui: ImGui's
    // Win32 backend hooks messageHook, this uses the separate setEventCallback slot, and
    // imgui_impl_win32 returns 0 (not consumed) so both see the messages -- a property of vendored
    // code an upgrade could silently change, why --input-source-test asserts the two agree. SPLIT:
    // ImGui decides WHETHER gameplay may have input; this owns WHAT the input is.
    InputState input_;

    // This frame's answer to "who owns the keyboard and mouse", recomputed once per frame before
    // pushInput. Consumers READ this rather than re-derive it from ImGui flags (InputOwnership.hpp's
    // eleven-spellings problem). Migrated consumer by consumer: one wrong central function beats
    // eleven independently wrong ones.
    editor::InputOwnership own_;

    // Set when a viewport click hands the mouse back to the game, cleared when that button comes up.
    // Its whole job is to stop ONE physical click counting as both a UI action and a trigger pull.
    bool eatRecaptureClick_ = false;

#if AVER_WITH_IMGUI
    editor::UiRegistry uiReg_;   // what the editor drew this frame, by name
#endif
#if AVER_MODULE_MCP
    mcp::McpBridge mcp_;         // inert until --mcp asks for it
    u16  mcpPort_ = 0;           // 0 = never asked for
#if AVER_MODULE_SCENE && AVER_MODULE_SCRIPTING
    // State for the "graph" ABI's load/attach/tick trio: load stages a path, attach binds+compiles it
    // onto a caller-chosen entity via scripts_.graphLoad, tick re-drives that entity's graph with a
    // caller-supplied time. One graph at a time: a debugging/proving seam, not a general manager.
    std::string  mcpGraphPath_;
    scene::Entity mcpGraphEntity_ = scene::kInvalidEntity;
    f32          mcpGraphTimeSeconds_ = 0.0f;
#endif

    void applyMcpCommand(const mcp::Command& c);

    void registerMcpAbis();

    // The "level" ABI (SandboxMcp.cpp): assembles a level through the editor's own spawn, transform,
    // material, collide, visibility, animation, delete, selection and save paths, so a scripted edit
    // is undoable and dirties the level exactly as a click does. Each refuses with a reason in `why`.
    bool mcpLevelAbi(const mcp::AbiCall& a, std::string& result, std::string& why);
    bool mcpLevelPlace(const std::string& text, std::string& result, std::string& why);
    bool mcpLevelSave(const std::string& target, std::string& result, std::string& why);
    bool mcpLevelMove(AvId e, const EditXform& x, std::string& why);
    bool mcpLevelEntity(f64 id, AvId& out, std::string& why) const;

    // STARTS THE CHANNEL, WHOLE: register the ABIs, install the widget hooks, then listen -- a
    // button doing only the last would bring up a channel that answers ping and nothing else. onInit
    // calls this too, rather than keeping its own copy.
    // REGISTRATION HAPPENS ONCE: starting/stopping/starting again must not re-register every ABI;
    // mcpAbisRegistered_ makes that safe. Returns false and logs when the listener can't bind --
    // the editor is unaffected either way.
    bool mcpStart(u16 port);
    void mcpStop();
    bool mcpAbisRegistered_ = false;
#endif // AVER_MODULE_MCP
    bool releasedByUser_ = false;   // Shift+F1 during a session; cleared when the session ends

    // ---- PLAY OPTIONS: the Play button's dropdown, laid out like Unreal's ----
    // Declared unguarded so the toolbar compiles in every configuration; the bodies (SandboxPlay.cpp)
    // guard themselves on AVER_MODULE_FRAMEWORK. Persisted in editor.ini by load/saveEditorPreferences
    // under play.mode / play.spawnAt / play.gameGetsMouse / play.standaloneArgs.
    //
    // No "New Editor Window", Mobile/VR preview or multiplayer net modes: the renderer has one
    // swapchain, there are no mobile/VR targets, and modules/net is a skeleton not in the build.
    enum class PlayMode : u8 {
        SelectedViewport = 0,   // play in the Level viewport, possessing the player (Alt+P)
        Simulate         = 1,   // the game runs, the player is not possessed, the editor keeps the camera (Alt+S)
        Standalone       = 2,   // a separate AverEngineRuntime.exe on the saved level (launchInRuntime)
    };
    enum class PlaySpawnAt : u8 { PlayerStart = 0, CameraLocation = 1 };
    PlayMode    playMode_ = PlayMode::SelectedViewport;    // last launched; the main button repeats it
    PlaySpawnAt playSpawnAt_ = PlaySpawnAt::PlayerStart;
    // WITH NO GAMEMODE THE DEFAULT PAWN CAN WALK (Play options > "Walk"; pref play.defaultPawnWalk; --play-walk):
    // a physics capsule character -- gravity, 40 cm stair steps, Space jumps, Shift runs, eye at 1.6 m -- so a
    // level's stairs, decks and bridges can be walked by hand without writing a GameMode. Off (the default) is the
    // flying spectator. walkCapsule_ is that character while Play runs, 0 otherwise.
    bool        defaultPawnWalk_ = false;
    int32_t     walkCapsule_ = 0;
    bool        playGameGetsMouse_ = true;   // capture the mouse the moment a viewport session starts
    std::string playStandaloneArgs_;         // extra command line appended for Standalone Game
    // Ejected (F8): the session keeps running but the editor has the camera, input and tools back;
    // the camera stops following the pawn and the pawn's owner-hidden body shows. Possess (F8 again)
    // snaps back. Simulate is a session that starts ejected. Cleared by stopPlay.
    bool        playEjected_ = false;
    // Frame Skip while paused: unpause for exactly one gameplay tick, then pause again.
    bool        playFrameStepPending_ = false;
    bool        scrollPrefsToPlay_ = false;  // "Advanced Settings..." scrolls Preferences to Play, once
    // Starts `m` (refused while any play is active, like the Play button) and remembers it as the
    // main button's mode. Standalone goes through launchInRuntime and its unsaved-changes prompt.
    void launchPlay(Engine& e, PlayMode m);
    // PLAY FROM HERE (viewport right-click): a viewport Play whose pawn's feet start at `surface` (+ a
    // few cm) facing the camera's yaw. playFromHere_ is the ONE-SHOT startPlay consumes; it never
    // touches playSpawnAt_ or playMode_.
    void playFromHere(const Vec3& surface);
    std::optional<Vec3> playFromHere_;
    // F8. Only meaningful in a real framework session (playSessionActive()); a no-op otherwise.
    void togglePlayEject();
    // Shift+F while ejected: the possessed pawn comes to the editor camera (a standing pawn's feet one
    // eye height below it, capsule included), so F8 resumes play where the camera flew to. A no-op in
    // any other state.
    void teleportPawnToCamera();
    // Frame Skip. Only while the session is paused.
    void requestPlayFrameStep();
    // True while a real session runs ejected.
    bool playEjected() const;
    // True while controller 0 possesses a pawn that is a live entity (Pawn to Camera's precondition).
    bool hasPossessedPawn() const;

    void setMouseCaptured(bool on);
    void pollCapturedMouse();

    bool playSessionActive() const;
    // Output Log capture. Written by logSink from any thread under logMutex_, read by the panel.
    static constexpr size_t kMaxLogLines = 4000;
    std::mutex          logMutex_;
    std::deque<LogLine> logLines_;
    // The on-screen graph-print feed. `at` is negative until the first frame that draws the line,
    // which then stamps it -- see logSink for why the sink cannot take the time itself.
    struct GraphPrint { std::string text; f64 at; u32 count; };
    std::deque<GraphPrint> graphPrints_;
    bool                logAutoScroll_ = true;
    int                 logLevelFilter_ = 0;      // 0 = all, 1 = Info+, 2 = Warn+
    // Console: its OWN scrollback, never logLines_ -- sharing the engine-wide log firehose would
    // defeat the point of a REPL transcript. Touched only from the UI thread inside drawConsole(), so
    // unlike logLines_ it needs no mutex.
    static constexpr size_t kMaxConsoleLines = 2000;
    std::deque<LogLine>      consoleLines_;
    bool                      consoleAutoScroll_ = true;
    bool                      consoleFocusPending_ = false;   // armed by toggleDrawer(Console)
    char                      consoleInput_[256] = {};
    std::vector<std::string>  consoleHistory_;                // command strings, most-recent-last
    int                       consoleHistoryPos_ = -1;         // -1 = not currently recalling history
    // Browse Variables tab's filter box (WHAT TO BUILD item 3) -- separate from cbFilter_ above,
    // which filters the unrelated Content Browser.
    char                      consoleBrowseFilter_[128] = {};
    // Content Browser: the folder whose files are listed, and the Import modal's source-path field.
    std::string         cbSelectedDir_;           // empty -> the content root
    char                importPath_[512] = {};
    bool                cbGallery_ = true;        // tiles vs list
    f32                 cbTileSize_ = 88.0f;      // gallery tile edge, in dp
    std::string         cbSelectedFile_;          // the highlighted entry in the file view
    char                cbFilter_[128] = {};      // the search box: filters the open folder by name
    // Whether the search box also looks under the open folder. See cbGatherDeepMatches; only ever
    // consulted while a filter is actually typed, or it would flatten the tree the browser is for.
    bool                cbSearchDeep_ = false;
    std::string         cbEngineRoot_;            // empty in a shipped build; the root is not offered
    std::vector<std::string> cbHistory_;
    int                 cbHistoryPos_ = -1;
    // The right-click target, which is not necessarily what is selected.
    std::string         cbContextPath_;
    bool                cbContextIsDir_ = false;
    // Deferred popup requests, acted on at panel level.
    bool                cbWantRename_ = false, cbWantDelete_ = false, cbWantNewFolder_ = false;
    bool                cbWantDuplicate_ = false, cbWantImport_ = false;
    char                cbRenameBuf_[256] = {};
    char                cbNewFolderBuf_[128] = {};
    std::string         cbStatus_;                // last operation's outcome, shown in the footer
    int                 cbIdeChoice_ = -1;        // -1 = whatever IdeIntegration prefers
    // The bottom drawers. drawerAnim_ is the eased 0..1 slide; drawerShown_ survives the retraction.
    Drawer              drawer_ = Drawer::None;
    Drawer              drawerShown_ = Drawer::Content;
    f32                 drawerAnim_ = 0.0f;
    f32                 drawerFrac_ = 0.48f;      // drawer height, as a fraction of the work area
    f32                 drawerRate_ = 14.0f;      // slide easing rate; higher is snappier
    bool                cbDoubleClickEnter_ = true;   // double-click a folder to enter it (vs single)
    bool                drawerRaise_ = false;     // focus it on the frame it opens, so it is on top
    // --drawer content:<sub> or console:<sub>, applied ONCE at first draw and cleared -- generic
    // across drawer kinds because only one drawer body ever runs in a given process (drawer_ picks
    // which), not because they interpret it the same way:
    // Content Browser treats it as a folder path, the console (drawConsoleTranscriptTab) as input-box seed text.
    std::string         drawerStartSub_;
    // ---- Revision control: what git said, latched off the frame thread ----
    // THE FRAME NEVER SPAWNS git: runCaptured (ProcessRun.hpp) waits INFINITE on its child, so a
    // call from inside a draw would freeze the editor for as long as git takes (cold index, huge
    // tree, network drive). Every query runs on a worker; the draw reads only what a FINISHED worker
    // left behind, so a missing or slow git costs the frame nothing.
    // THE WORKER IS DETACHED, NOT JOINED (unlike ToolsMenu's compile thread, whose result MUST be
    // reaped -- the assembly swap happens there): a status refresh has no such obligation, and
    // joining on exit would wait on the slow git this exists to avoid. Safe because the worker
    // captures its job by shared_ptr, never `this`: one still running after SandboxApp is gone
    // writes into memory it co-owns, then exits.
    // gitAvailable() IS ASKED ONLY FROM THE WORKER: it's a magic static, so the first caller pays for
    // a `git version` child process -- asking from a draw would put that one blocking spawn into the frame.
    struct RcStatusQuery {
        std::atomic<bool> done{false};
        std::string dir;                  // the project directory asked about; checked on reap
        std::string root;                 // git's spelling of the toplevel; empty = not a repository
        editor::RepoStatus status;
        std::string why;                  // git could not be asked; EMPTY for "not a repository"
        bool gitPresent = false;
    };
    // One path's history and one side's diff, fetched together: the panel shows both for the row
    // that was clicked, and two workers for one click would let the two halves disagree about
    // which file is on screen.
    struct RcFileQuery {
        std::atomic<bool> done{false};
        std::string root;                 // the repository asked; checked on reap
        std::string path;                 // repo-relative, git's spelling
        std::vector<editor::LogEntry> log;
        std::string logWhy;
        // SPLIT ON THE WORKER, not the draw. The viewer clips to what is visible, but splitting
        // a megabyte of diff into lines every frame would be the same frame cost this mechanism
        // avoids, just moved from a process spawn to a string walk.
        std::vector<std::string> diff;
        std::string diffWhy;
        bool wantDiff = false;            // false for a file the viewer has nothing to say about
    };
    std::shared_ptr<RcStatusQuery> rcStatusJob_;
    std::shared_ptr<RcFileQuery>   rcFileJob_;
    bool                showRevisionControl_ = false;   // Window > Revision Control
    std::string         rcProjectDir_;        // the project the latched answer describes
    std::string         rcRoot_;              // repository root, git's spelling
    // rcRoot_ lower-cased with forward slashes and no trailing separator: what rcKeyFor compares
    // an editor path against. Precomputed at latch time because the Content Browser asks it once
    // per visible card per frame, and Windows paths differ in case without differing at all.
    std::string         rcRootKey_;
    editor::RepoStatus  rcStatus_;
    std::string         rcWhy_;
    bool                rcAnswered_ = false;  // a query has completed for rcProjectDir_ at least once
    bool                rcGitPresent_ = false;
    f64                 rcRefreshedAt_ = -1.0;  // ImGui::GetTime() of the last LATCH; -1 = never
    // The badge table: one entry per changed path, sorted by path so both the exact-file lookup and
    // a folder's prefix range are a binary search rather than a walk of the whole list.
    std::vector<std::pair<std::string, editor::FileStatus>> rcMarks_;
    // The row the panel is showing history and a diff for.
    std::string         rcSelected_;
    std::vector<editor::LogEntry> rcLog_;
    std::vector<std::string>      rcDiff_;
    std::string         rcLogWhy_, rcDiffWhy_;
    bool                rcSelectedDiffable_ = false;   // a text asset the diff viewer can show
    // Which of the selected path's two diffs is on screen. A file can have BOTH -- staged as added
    // and then edited again is one path with two different answers (see FileEntry's own comment) --
    // so this is a choice the panel has to offer rather than derive.
    editor::DiffSide    rcDiffSide_ = editor::DiffSide::Worktree;
#if AVER_MODULE_SCENE
    // Mesh handles, bounds and per-material parts are content_'s (meshFor, boundsFor, partsFor).
    std::unordered_map<u64, std::string>     meshPathById_;   // id -> project-relative path
    // THE TWO WAYS THE SCENE WALK DROPS AN ENTITY, each reported once, keyed differently on purpose:
    // the invisible-bit fault belongs to an ENTITY (mis-seeded component), an unresolved id belongs
    // to the ID (every entity naming it shares one fault). Never cleared on level unload -- a
    // reload's report is the same fault.
    std::unordered_set<u64> undrawnInvisible_;
    std::unordered_set<u64> undrawnMissingMesh_;
    // Triangle count per mesh id, same key as content_'s meshes. Exists so a resident triangle
    // BUDGET can be reported instead of guessed -- otherwise a scatter palette can only be tuned "by looking".
    std::unordered_map<u64, u32> meshTris_;
    // The .ocmesh ids that carry skin weights -- a SKELETAL mesh, Unreal's vocabulary, coloured
    // differently in the Content Browser. Recorded rather than re-read: loadProjectMeshes has already
    // parsed the file by the time it knows this, otherwise reachable only by opening every mesh.
    std::unordered_set<u64> skinnedMeshIds_;
    // Per-mesh triangle data for pick() (ViewportPick.hpp), keyed like sceneMeshes_/meshBounds_.
    // Built-ins are filled at creation, above (:2113-2142); a project mesh id is filled LAZILY by
    // pickGeometryFor, the first time a click ray reaches it -- loadProjectMeshes discards its
    // CPU-side OcMeshData once the GPU upload is done (same reason selOutlineLines_ re-reads on
    // demand), so paying for every mesh's positions/normals/indices up front would waste memory on
    // meshes no click ever tests. An EMPTY
    // PickGeometry means "tried and unavailable" -- pick() falls back to the bounding box, and the
    // empty entry stops a failed load from being retried on every later click.
    std::unordered_map<u64, aver::editor::PickGeometry> pickGeometry_;
    int lastSceneDrawn_=-1;           // last scene-entity draw count (startupComplete's settle input; the
                                      // scene-render log line's own throttle is g_sceneRenderLog)
    // startupComplete's settle detector; see it for why these are mutable and why a frame count.
    mutable int startupSettleCount_ = -2;   // -2 so it cannot match lastSceneDrawn_'s -1 start
    mutable int startupSettleFrames_ = 0;
// THE LOADING SCREEN IS NOT SCENE STATE: landed here by proximity to the settle counters it's
// dismissed by. Neither member names a scene type, and all five use sites (the log sink in
// SandboxApp.cpp, applyProject in SandboxProject.cpp, the dismiss check in SandboxRender.cpp) are
// unguarded -- a project opens, streams assets and wants a splash whether or not this tree has an
// ECS. Guard closes/reopens around the pair rather than moving them, so no member changes position
// relative to any other.
#endif  // AVER_MODULE_SCENE
    // The project-open loading screen, alive from applyProject until the scene settles. Null the
    // rest of the time; see applyProject for why it is not a local any more.
    std::unique_ptr<struct LoadingScreen> projectLoading_;
    // Frames the OWNED project loading screen has been up; see onRender for why it is capped.
    int projectLoadingFrames_ = 0;
#if AVER_MODULE_SCENE
    int lastSceneCulled_=-1;          // and the cull count, so a frustum bug shows as a number rather than a gap
    int lastSceneOwnerHidden_=-1;     // and the owner-hide count, so a stuck `hidden=owner` mesh shows as a number too
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    // Per-mesh LOD ladder for virtualized-geometry selection. Populated in loadProjectMeshes ONLY for
    // a mesh the Cook wrote coarserLods for (lodCount() > 1); no entry means it always draws LOD-0.
    // Every vector is indexed by LEVEL: [0] mirrors sceneMeshes_[id]/meshTris_[id], [i>0] is the
    // Cook's coarserLods[i-1] -- its own index buffer against the SAME vertex buffer level 0 uses,
    // so this only ever duplicates INDEX data, never vertices.
    struct MeshLodLadder {
        std::vector<rhi::MeshHandle> handles;   // [level] -> whole-level MeshHandle
        std::vector<u32> triCounts;              // [level] -> that level's own triangle count
        std::vector<f32> errorCm;                // [level] -> aver::trifactor::levelWorldErrorCm(mesh, level)
        // [level] -> that level's meshlets, decoded to ClusterView (bounds + cone only, no vertex
        // data) -- kept resident so the per-frame pass can run the real, tested
        // selectVisibleClustersWithStats for informational telemetry on the level actually chosen,
        // without re-parsing the .ocmesh. Counted but NOT (yet) subtracted from what's drawn.
        std::vector<std::vector<trifactor::ClusterView>> clusters;
    };
    std::unordered_map<u64, MeshLodLadder> meshLods_;
#endif

    // THE TWO KNOBS ARE OUT OF THE GUARD; THE LADDER THEY DRIVE STAYS IN IT: RENDER.LODSELECT and
    // RENDER.LODTHRESHOLDPX round-trip through applyProjectRenderSettings/captureRenderSettingsFromUi,
    // gated on VOXI not this module, so a TRIFACTOR=OFF tree would otherwise lose members the project
    // round-trip and Rendering page checkbox still read -- same trade as occlusionCullEnabled_
    // above: guarding those uses instead would make an edit on a build without the Cook rewrite a
    // manifest key it cannot honour, worse than the compile error it replaces.
    // ON by default: measured cost of OFF was every instance drawing LOD 0 regardless of distance,
    // the exact thing the Cook builds a ladder to avoid. --no-lod-select restores the old behaviour.
    // See setLodSelect for the numbers.
    bool lodSelectEnabled_ = true;      // --lod-select / editor toggle. OFF reproduces pre-existing
                                        // behaviour EXACTLY: every instance draws sceneMeshes_[id]
                                        // (LOD 0), the same handle and code path as before this file.
    f32  lodErrorThresholdPx_ = 1.0f;   // --lod-error-px <n>; pixels of projected screen error
                                        // tolerated before a coarser level is preferred. Same unit
                                        // ClusterSelect.hpp's inCut/screenSpaceErrorPx compare against.
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    // --lod-cluster-stats: OFF by default, on purpose: gates ONLY the informational per-meshlet
    // frustum/cone-cull telemetry below (a real, extra per-instance CPU cost) -- kept separate from
    // lodSelectEnabled_ so the primary --lod-select frame-time comparison excludes this cost.
    bool lodClusterStatsEnabled_ = false;
    // This frame's selection counters, logged under "[LOD-SELECT]" whenever any changes.
    // trianglesBeforeLod0 vs trianglesAfterLevel predicts frame time (every "after" triangle reaches
    // a drawMesh call); cluster-cull counters are real telemetry but NOT subtracted from "after" --
    // this slice selects per-level, not per-cluster.
    struct LodSelectStats {
        u32 instancesTested = 0;
        u32 levelCollapsed = 0;        // instances that drew level > 0 this frame
        u32 clustersTested = 0;        // sum of chosen-level meshlet counts, over drawn instances
        u32 frustumCulled = 0;         // informational -- see struct comment
        u32 coneCulled = 0;            // informational -- see struct comment
        u64 trianglesBeforeLod0 = 0;   // sum of LOD-0 triangle counts, as if every instance drew level 0
        u64 trianglesAfterLevel = 0;   // sum of the CHOSEN level's triangle counts -- what actually draws
    };
    LodSelectStats lodStats_{};
    LodSelectStats lastLoggedLodStats_{};

    // PER-CLUSTER selection -- what makes this virtualized geometry instead of discrete LOD with
    // generated levels (ClusterAdapt.hpp's "PER-CLUSTER, at last"). Switchable against the per-level
    // path above (--lod-per-cluster), one flag on the same build -- both paths ship in every binary.

    // Every meshlet of a mesh across EVERY LOD level, mesh-local space, plus its expanded GLOBAL
    // triangle-index list -- built once at load, same gate as MeshLodLadder. `verts` is a COPY of
    // the vertex array createMesh was first called with; every cluster from every level indexes it,
    // so ONE copy serves the whole DAG and every cut an instance can select.
    struct MeshClusterData {
        std::vector<trifactor::MeshClusterView> clusters;   // mesh-local; all levels
        std::vector<std::vector<u32>> clusterIndices;        // [clusterId] -> expanded global indices
        std::vector<rhi::MeshVertex> verts;

        // THE INSTANCE-LEVEL SHORTCUT's own precomputed inputs (see ClusterAdapt.hpp's section for
        // the full derivation). Both built ONCE here, from `clusters` right above, never touched again
        // per frame -- O(clusters.size()), paid once at load.
        std::vector<trifactor::MeshClusterLevelBounds> levelBounds;
        f32 maxSphereRadius = 0.0f;   // mesh-local; largest cluster sphere radius across the whole DAG
    };
    std::unordered_map<u64, MeshClusterData> meshClusterData_;

    // ONE cut-assembled MeshHandle PER INSTANCE (not per mesh: two instances of the same mesh at
    // different distances select different clusters), rebuilt only when the selected cluster-id set
    // changes. Keyed by scene::Entity; `lastUsedFrame` is how staleEntityCacheSweep reclaims a handle
    // whose entity stopped appearing, without hooking every destruction explicitly.
    struct ClusterCutCache {
        std::vector<u32> selectedIds;   // sorted; last frame's cut, for the cheap same-cut check
        rhi::MeshHandle handle = 0;     // 0 = none yet, or the selection was empty
        u64 lastUsedFrame = 0;
        u64 rebuildCount = 0;           // how many times THIS instance's handle was actually rebuilt
    };
    std::unordered_map<scene::Entity, ClusterCutCache> clusterCutCache_;
    u64 lodClusterFrame_ = 0;           // incremented once per onRender call; drives the sweep below
    bool lodPerClusterEnabled_ = false; // --lod-per-cluster. OFF: the per-LEVEL path above runs
                                        // unchanged, exactly as if this whole section did not exist.

    // Measured cost of the CPU-assembly upload this design's report flags as the failure mode to
    // watch (ClusterAdapt.hpp, "MEASURE the upload"). Real std::chrono timing around every createMesh
    // call this path makes, not a guess.
    struct LodClusterStats {
        u32 instancesTested = 0;
        u32 clustersTested = 0;
        u32 frustumCulled = 0;
        u32 coneCulled = 0;
        u32 lodRejected = 0;
        u32 clustersDrawn = 0;
        u64 trianglesBeforeLod0 = 0;
        u64 trianglesDrawn = 0;
        u32 instancesMixedLevels = 0;   // instances whose OWN cut spans > 1 distinct level this frame
        u32 maxDistinctLevelsSeen = 0;  // the single highest distinct-level count seen on any instance
        u32 rebuilds = 0;               // cache misses (cut changed) this frame, across all instances
        u32 cacheHits = 0;              // cache hits (cut unchanged) this frame
        u64 rebuildIndices = 0;         // sum of assembled index counts on rebuilds this frame
        f64 rebuildMs = 0.0;            // wall time spent inside createMesh on rebuilds this frame
        u32 instancesShortcut = 0;      // provablySingleLevelCut fired: the O(all-DAG-clusters) scan,
                                         // the copy and the transform were all skipped for this instance
    };
    LodClusterStats lodClusterStats_{};
    LodClusterStats lastLoggedLodClusterStats_{};

    // GPU per-cluster LOD -- --lod-mesh-shader. An amplification shader (ASMain) runs the SAME local
    // cut test as selectClusterLocal/inLocalCut above, one thread per cluster; a mesh shader
    // (MSClusterMain) expands each surviving cluster's block (RHIShaders.cpp's AVER_MS_CLUSTER,
    // ClusterAdapt.hpp's buildMeshClusterGpuData). Exists ALONGSIDE the CPU per-cluster path, never
    // replacing it: that path stays the correctness reference the two are checked against, and is
    // what a mesh-shader-tier-0 device still runs when this flag is on.

    // Per-mesh GPU cluster buffers, built once at load time alongside MeshClusterData -- ONLY when
    // --lod-mesh-shader is on, so a run that never asks never pays the extra upload. `clusterCount` is
    // outBounds.size() from buildMeshClusterGpuData -- every level's clusters, concatenated.
    struct MeshClusterGpu {
        rhi::BufferHandle bounds = 0;   // ClusterBounds[] -- t0
        rhi::BufferHandle desc   = 0;   // ClusterMeshletDesc[] -- t1
        rhi::BufferHandle verts  = 0;   // flat global vertex indices -- t2
        rhi::BufferHandle tris   = 0;   // flat packed local triangles -- t3
        rhi::BindingSetHandle bindingSet = 0;
        u32 clusterCount = 0;
    };
    std::unordered_map<u64, MeshClusterGpu> meshClusterGpu_;
    bool lodMeshShaderEnabled_ = false;   // resolved in onInit; see lodMeshShaderRequest_
    // -1 = decide from DeviceCaps (default), 0 = --no-lod-mesh-shader, 1 = --lod-mesh-shader.
    // Unguarded like chunkStreamAutoFrames_: the setter runs from unguarded flag parsing, so the
    // member must exist in every configuration even where nothing reads it.
    int lodMeshShaderRequest_ = -1;

    // Pipeline creation is lazy (first frame the flag is on and a device exists) and tried EXACTLY
    // ONCE per run: a failed compile or a tier-0 device means "use the CPU path instead", logged once,
    // not retried every frame -- the degrade house rule 6 asks for, at the feature level.
    bool lodMeshPipelineTried_ = false;
    bool lodMeshPipelineReady_ = false;
    // The pbr::MaterialGraphRegistry revision the cluster pipeline was last compiled against. See
    // ensureLodMeshPipeline, which reopens its own once-only latch when this moves.
    u64  lodMeshPipelineGraphRev_ = ~0ull;
    rhi::ShaderHandle lodMeshAsShader_ = 0, lodMeshMsShader_ = 0, lodMeshPsShader_ = 0;
    rhi::PipelineHandle lodMeshPipeline_ = 0;
    rhi::PipelineLayout lodMeshLayout_{};   // srvCount=4 (the four cluster buffers); kept so
                                             // meshGeometryDefines(layout) at draw time (were it ever
                                             // needed again) agrees with what compiled the shaders.

    // Per-instance constant block for ASMain/MSClusterMain beyond gWorld/gViewProj/gCamPos --
    // byte-for-byte the HLSL ClusterFrameCB, bound as a root CBV at b4. `planes` is
    // Frustum::fromViewProj's own output, copied verbatim, so the GPU test runs against the IDENTICAL six numbers the CPU reference tests against.
    struct ClusterFrameCB {
        f32 budgetPx = 0.0f;
        f32 projScale = 0.0f;
        f32 worldScale = 1.0f;
        u32 _pad = 0;
        f32 planes[6][4] = {};
    };

    // Informational counters for --lod-mesh-shader ("[LOD-MESH-SHADER]"). NOT a GPU readback: runs
    // the same already-tested CPU reference over the same clusters/budget the GPU dispatch used.
    // Since ASMain/MSClusterMain are byte-for-byte CPU ports, this counts what the GPU actually drew
    // -- but it's still CPU arithmetic, said plainly in the report.
    struct LodMeshShaderStats {
        u32 instancesTested = 0;
        u32 clustersDispatched = 0;      // sum of MeshClusterGpu::clusterCount over drawn instances
        u32 survivors = 0;               // sum of survivor counts (CPU-mirrored, see struct comment)
        u64 trianglesDrawn = 0;
        u32 instancesMixedLevels = 0;
        u32 maxDistinctLevelsSeen = 0;
        u32 instancesShortcut = 0;       // of the sampled instances, how many used
                                          // provablySingleLevelCut instead of the real CPU-mirror scan
                                          // -- matters far more here than in the CPU per-cluster path's
                                          // count: without it, this "sampled every 64 frames" telemetry was itself a multi-SECOND stall once every 64 frames.
    };
    LodMeshShaderStats lodMeshShaderStats_{};
    LodMeshShaderStats lastLoggedLodMeshShaderStats_{};
#endif

    // OUTSIDE EVERY MODULE GUARD, and it has to be: POPULATED beside the Trifactor LOD ladder, but
    // READ from the unguarded call handing the resolver to Voxi -- declaring it next to what fills
    // it broke both scene-off and trifactor-off. The guard belongs where a thing is BUILT, never
    // where it's declared. Degrading is the point: with no Trifactor the map is empty, every lookup
    // answers 0, and every pass draws the mesh it was given.
    std::unordered_map<rhi::MeshHandle, rhi::MeshHandle> depthProxy_;

    static rhi::MeshHandle depthProxyLookup(rhi::MeshHandle mesh, void* user);

    u32 sceneWalkReports_ = 0;     // scene walks so far; the cost split reports at 2^n of them

    Mat4 invVP_, viewProj_; Vec3 eye_{0,0,0};
};

} // namespace aver
