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
#include "aver/core/CrashReport.hpp"
#include "aver/core/Assert.hpp"
#include "aver/core/Math.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Version.hpp"
#include "aver/formats/OcBeam.hpp"
#include "aver/formats/OcMap.hpp"
#if AVER_WITH_AUDIO_ABI
// Historical name: set whenever Aver.Audio.Abi links in, meaning "this build has the mixer seam", not
// "the sound editor wants audio" (named when SoundEditor's preview button was the only thing that
// opened a device). Not renamed to AVER_SANDBOX_AUDIO because tests/editor relies on it undefined.
#  include "aver/audio/audio_abi.h"
#endif
#include "aver/formats/OcProject.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/formats/OcMesh.hpp"
// The behaviour-tree format, for the Content Browser's "New Behaviour Tree". .ocbt is a binary
// AVR1 container, not a text format, so a starter has to go through fmt::saveOcBt rather than
// being written as lines the way a starter .ocgraph is.
#include "aver/formats/OcBt.hpp"
#include "aver/formats/GltfImport.hpp"
#if AVER_MODULE_TRIFACTOR
#include "aver/trifactor/ClusterAdapt.hpp"
#endif
// The landscape runtime: a complete quadtree-LOD heightfield renderer that, before this change,
// nothing outside tests/landscape linked. See the member block near landscape_ below for how it
// is hosted -- independent of AVER_MODULE_SCENE, since a section is not an ECS entity.
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
#if AVER_MODULE_SCENE
#include "aver/render/SkinnedScene.hpp"
#include "ThumbnailCache.hpp"
#endif
#include "aver/ui/ui_abi.h"

#include "ProjectBrowser.hpp"
#include "LevelClassSave.hpp"
#include "LevelList.hpp"
#include "ProjectScaffold.hpp"
#include "GraphAssetPresentation.hpp"
#include "MaterialResolve.hpp"
#include "ClusterMaterialShader.hpp"
// F1 (occlusion-fix-plan.md): the ONE place the "which route delivers this entity's draws" rule
// lives -- see that header's own top comment. A pure header, like PtRenderConflict.hpp, so
// aver::editor::SurfaceLook below is a DIFFERENT type from this file's own (unqualified) SurfaceLook
// a few thousand lines down (24819) -- always write aver::editor::SurfaceLook in full here; SandboxApp
// is declared in namespace aver, not aver::editor, so the two never collide as types, but an
// unqualified `SurfaceLook` inside a SandboxApp member function resolves to the nested one every time
// (class-scope lookup wins over a namespace one), never to this header's.
#include "SceneSubmission.hpp"
// The pick()'s-eye view of a mesh (positions/normals/indices) and its nearest-hit ray/triangle test --
// see ViewportPick.hpp's own top comment. A second pure header for the same reason SceneSubmission.hpp
// is one: pick()'s ray/triangle math is otherwise untestable inside a 29,000-line file with no header
// of its own.
#include "ViewportPick.hpp"

// The material sampler register on the cluster pipeline (materialShaderDefines() gets the same
// number). Fixed at s0 so it never moves whether or not AVER_MODULE_VOXI is compiled in -- Voxi's own
// volume/shadow samplers start at s1 instead of reusing s0/s1.
namespace { constexpr aver::u32 kClusterMaterialSamplerSlot = 0; }
#if AVER_MODULE_VOXI
// Stage 3: where the GPU per-cluster pipeline's table 0 puts Voxi's MERGED GI/shadow resources,
// relative to the 4 cluster-geometry SRVs (t0..t3) always there. Full register map in
// ensureLodMeshPipeline; consumers in VoxiGiShaders.hpp.
namespace {
constexpr aver::u32 kClusterGiSrvBase       = 4;   // t4 the GI volume, t5 the shadow map
constexpr aver::u32 kClusterGiSamplerBase   = 1;   // s1 volume (linear-clamp), s2 shadow (comparison)
// b3, NOT kFeatureFrameConstantRegister (b4): the AS/MS half of this SAME pipeline already owns b4
// for ClusterFrameCB, and a root signature has one cbuffer per register regardless of stage. b3 is
// free (see ensureLodMeshPipeline's constantDwords[3]).
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
#include "AssetEditor.hpp"
#include "ActorEditor.hpp"
#include "AnimEditor.hpp"
#include "GraphEditor.hpp"
#include "BtEditor.hpp"
#include "SoundEditor.hpp"
// Guarded: ParticleEditor.hpp unconditionally #includes aver/formats/OcParticle.hpp (Aver.Formats.
// Particles), which sandbox/CMakeLists.txt links only `if(TARGET Aver.Formats.Particles)` -- itself
// gated on Aver.Particles existing at all. A tree configured with AVER_MODULE_PARTICLES=OFF has
// neither target, so the header is unreachable and every reference to this tab must be guarded the
// same way (the factory registration, the shutdown call, and the Content Browser's create-menu entry
// below), matching the `#if AVER_MODULE_PARTICLES` already used everywhere else in this file for the
// scene-component half of this same optional module.
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

#if AVER_MODULE_VOXI
#include "aver/voxi/Voxi.hpp"
#include "aver/voxi/VoxiRenderer.hpp"
// Stage 3: the narrow GI/shadow HLSL slice the GPU per-cluster pipeline composes into
// ClusterMaterialShader.hpp's PSClusterMain. See its own header comment for what this is and is not.
#include "aver/voxi/VoxiGiShaders.hpp"
// ProjectRenderApply.hpp: the shared, header-only two-phase manifest apply (Lane 2 of the settings-
// separation pass) -- applyProjectVoxiSettings and captureRenderSettingsFromUi below are both thin
// callers of it now. Pulls in QualityLadder.hpp, RenderSettingsResolver.hpp (voxi::resolve, used
// throughout buildRenderingSettings and the G-buffer switch) and Scalability.hpp (the Overall Quality
// preset) along the way, so this one include is every settings-separation header this file needs.
#include "aver/voxi/ProjectRenderApply.hpp"
#include "aver/voxi/FrameBudget.hpp"
// NrdDenoiser.hpp, for Denoiser::available() alone -- the DeviceInfo::nrdSupported computation just
// below is R1's other mirror site (GameApp::attachVoxi, Runtime/src/GameApp.cpp, is the
// first; same expression, `backend() == D3D12 && available()`). Aver.Render.Voxi.Renderer links
// Aver.Render.NRD PUBLIC (modules/render.voxi/CMakeLists.txt), and this file already links against
// the former for VoxiRenderer.hpp above, so the include needs no extra guard.
#include "aver/render/nrd/NrdDenoiser.hpp"
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

// Fluids and Soft-Body rendering are INDEPENDENT modules -- neither depends on Particles at all (see
// modules/fluids/CMakeLists.txt and modules/render.softbody/CMakeLists.txt) -- so each gets its own
// guard keyed on the module that actually owns it, matching the guards their own use sites below
// already use (AVER_MODULE_RENDER_SOFTBODY && AVER_MODULE_SCENE at softBodyScene_, AVER_FLUIDS_
// SIMULATED inside GameWater.hpp). b6881c49 nested all four headers inside
// `AVER_MODULE_PARTICLES && AVER_MODULE_SCENE` above, so a particles-off tree dropped these headers
// too while the members and calls they declare stayed compiled in -- ~40 errors with particles off.
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
#include "aver/assets/LevelSky.hpp"
#include "aver/assets/TextureUpload.hpp"
#endif

#if AVER_MODULE_SCRIPTING
#include "aver/scripting/ScriptHost.hpp"
#endif

// AverSR (docs/AVERSR.md): Sandbox is the composition root that links Aver.Render.Sr and constructs
// the concrete aver::sr::SpatialUpscaler; the generic RHI (aver/rhi/RHI.hpp) never does and never will.
#if AVER_MODULE_SR
#include "aver/sr/AverSrQuality.hpp"
#include "aver/sr/AverSrSpatial.hpp"
#include "aver/sr/AverSrFxaa.hpp"
// optimisation-wave-2, U2/3.2: render.voxi's own ladder constants (QualityLadder.hpp's
// kAverSrOff/kAverSrQuality/kAverSrBalanced/kAverSrPerformance, pulled in transitively through
// ProjectRenderApply.hpp above) mirror aver::sr::Quality's numbering ON PURPOSE, so
// Scalability.hpp's resolveAverSrLevel can hand a plain u32 back to a host that casts it straight to
// aver::sr::Quality with no translation table of its own -- render.voxi still never includes aver/sr
// itself (the module-boundary rule Scalability.hpp's own header comment states), so the ONE place
// that can check the two enums actually agree is a host that includes both, like this one. Caught
// here, at compile time, rather than as a level that silently renders at the wrong scale.
static_assert(static_cast<aver::u32>(aver::sr::Quality::Off)         == aver::voxi::ladder::kAverSrOff,
             "aver::sr::Quality::Off no longer matches aver::voxi::ladder::kAverSrOff");
static_assert(static_cast<aver::u32>(aver::sr::Quality::Quality)     == aver::voxi::ladder::kAverSrQuality,
             "aver::sr::Quality::Quality no longer matches aver::voxi::ladder::kAverSrQuality");
static_assert(static_cast<aver::u32>(aver::sr::Quality::Balanced)    == aver::voxi::ladder::kAverSrBalanced,
             "aver::sr::Quality::Balanced no longer matches aver::voxi::ladder::kAverSrBalanced");
static_assert(static_cast<aver::u32>(aver::sr::Quality::Performance) == aver::voxi::ladder::kAverSrPerformance,
             "aver::sr::Quality::Performance no longer matches aver::voxi::ladder::kAverSrPerformance");
#endif

// physics_abi.h was nested inside AVER_MODULE_FRAMEWORK, but every use site below is guarded on
// AVER_MODULE_PHYSICS alone -- invisible while physics implied framework, until SCENE=OFF forced
// FRAMEWORK off while leaving PHYSICS on, breaking the guarded call sites with no missing symbol.
#if AVER_MODULE_PHYSICS
#include "aver/physics/physics_abi.h"
#endif
#if AVER_MODULE_FRAMEWORK
#include "aver/framework/framework_abi.h"
#include "aver/framework/framework_hooks.h"
#endif

// windows.h was nested inside AVER_MODULE_SCENE, but the Win32 calls that need it (applyMcpCommand)
// are gated on _WIN32 alone, with no scene dependency -- SCENE off lost the header while that
// guarded call site still expected it.
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
// The divergence census both hosts print -- see SceneCensus.hpp for why it is a census of what the
// level loaded and not a comparison of frames.
#include "aver/world/SceneCensus.hpp"
// Opt-in chunk streaming around the editor camera. See SandboxApp::setChunkStreamingEnabled;
// aver/game/GameStreaming.hpp above already pulls in ChunkWorld.hpp under AVER_MODULE_SCENE.
// A level's SCATTER records -> the generator's palette. The editor does not do this conversion
// itself: the game runtime needs the identical one, and one of the two would drift.
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
// The seam + the concrete Dear ImGui backend that plugs into it (UiBackend.hpp / ImGuiUiBackend.hpp).
// Needed here, not just inside the RHI, because installing a backend is the one thing only the app
// (not the RHI, which must not know ImGui exists) can decide to do.
#include "aver/rhi/d3d12/UiBackend.hpp"
#include "aver/rhi/d3d12/ImGuiUiBackend.hpp"
#endif
#if AVER_WITH_IMGUI_VULKAN
// The same pair for Vulkan, on its own macro: a tree can build either backend, both or neither, so
// these cannot ride along on AVER_WITH_IMGUI, which means "the D3D12 backend's types are here".
#include "aver/rhi/vulkan/UiBackend.hpp"
#include "aver/rhi/vulkan/ImGuiUiBackend.hpp"
#endif

#include "stb_image_write.h"

#include <algorithm>
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

// The editor's window-event sink. A free function because Window::setEventCallback takes a plain
// function pointer and a void* (same shape GameApp's onWindowEvent uses). Filters nothing: whether a
// given CONSUMER may act on an event is a separate, per-frame question answered further down.
// Filtering here would put policy in the one place that cannot see it.
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


// Gizmo axis basis and colours: X red, Y green, Z blue, amber highlight.
static const Vec3 kAxisDir[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
static const Vec3 kAxisCol[3] = {{0.92f, 0.24f, 0.24f}, {0.36f, 0.82f, 0.30f}, {0.30f, 0.55f, 1.0f}};
static const Vec3 kAxisHi = {1.0f, 0.80f, 0.15f};

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

// Generalised appendBox: independent per-axis half-extents (hx,hy,hz), yawed by yawDeg around Z --
// appendBox only covers a symmetric, unrotated cube, and the drone's arms/body/skids need varied
// aspect ratios. Face table copied verbatim; yawDeg=0 with hx=hy=hz reproduces appendBox exactly.
static inline void appendBoxYaw(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx,
                          f32 cx, f32 cy, f32 cz, f32 hx, f32 hy, f32 hz, f32 yawDeg) {
    const f32 rad = yawDeg * kDegToRad;
    const f32 cs = std::cos(rad), sn = std::sin(rad);
    // Rotates a LOCAL (lx,ly,lz) around Z. A pure rotation has determinant +1, so it changes nothing
    // about winding or handedness -- every face below stays CCW-outward exactly as appendBox left it.
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

// Appends a capped cylinder along +Z -- the drone's motor pods and rotor discs. Flat-shaded per face
// like appendBox/appendBoxYaw, not smooth like appendSphere: at rotor-pod segment counts (8-10) a
// smoothed normal would look indistinguishable from a faceted one.
static inline void appendCylinderZ(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx,
                             f32 cx, f32 cy, f32 cz, f32 radius, f32 halfHeight, u32 segments) {
    for (u32 s = 0; s < segments; ++s) {
        const f32 a0 = kTwoPi * static_cast<f32>(s) / static_cast<f32>(segments);
        const f32 a1 = kTwoPi * static_cast<f32>(s + 1) / static_cast<f32>(segments);
        const f32 x0 = std::cos(a0), y0 = std::sin(a0);
        const f32 x1 = std::cos(a1), y1 = std::sin(a1);
        // Side quad: both edges get the SAME flat normal -- the averaged (renormalised) radial
        // direction of the two -- the same "one normal per face" rule appendBox uses, just computed
        // rather than hand-written because the direction depends on which segment this is.
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
        // Top (+Z) and bottom (-Z) caps, each a single fan triangle for this segment's wedge -- cheap
        // at these segment counts (an 8-10 sided cap still reads as round) and it keeps the caps flat-
        // shaded too, instead of introducing yet another normal convention for just two faces.
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

// The Euler <-> quaternion pair now lives in EditorEuler.hpp so it can be tested. It was two static
// functions here, unreachable from any test, and eulerDegFromQuat's gimbal branch was wrong in a way
// that wrote corrupted rotations into saved levels.
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
// Converts an interleaved MeshVertex/index pair into a PickGeometry -- used only for the file's three
// built-in meshes (sphere/cube/drone), whose CPU-side vertices already exist as this exact array at
// creation time. Project meshes never go through here: pickGeometryFor (below) builds theirs straight
// from fmt::OcMeshData's own separate position/normal arrays, which this would just have to
// un-interleave right back out of.
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

// Which mode the viewport is in: Select edits OBJECTS (pick+gizmo); Landscape edits TERRAIN (sculpt,
// no picking/gizmo) -- a mode, not another tool (sculpt tools reuse this shape; see handleSculpt()).
// USED TO BE ONE ENUM with Raise/Lower/Smooth/Flatten beside Move/Rotate, so gizmo and picking ran
// during terrain edits with no way to tell which activity a click meant. Foliage and Simulate
// (framework's play/pause/stop, AVER_FW_PLAY_*) were added because both were already real.
// Mesh Paint / Geometry-Modeling absent: OcMeshData has no vertex-colour/weight channel to paint, and
// "MeshEditor" is read-only -- add once the format and an editable mesh exist. An empty-panel mode is
// worse than none.
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
// The Player Start marker's world-space half-size, in centimetres. 45 rather than the cube's 50: the
// pin artwork is taller than wide with a transparent margin, so matching the cube's half-extent read
// noticeably BIGGER. Chosen by eye against a 100cm cube.
inline constexpr f32 kPlayerStartIconHalfSize = 45.0f;   // cm
inline constexpr f32 kEditorGridCell  = 100.0f;    // cm
inline constexpr f32 kEditorGridHalf  = 1000.0f;   // cm
// How far in front of the camera Add places a new object.
inline constexpr f32 kAddDistance     = 400.0f;    // cm
// Duplicate's fallback nudge off the original, when move-snap is off (snapped, it uses moveSnap_
// instead, so the copy always lands on the same grid the original does).
inline constexpr f32 kDuplicateOffset = 50.0f;     // cm

// Drag-drop payload carrying a content-browser item's full path as bytes (content browser ->
// viewport asset placement). Under ImGui's 32-char payload-type limit; not prefixed with '_'.
static constexpr const char* kAssetDragDropType = "AVER_ASSET_PATH";
// The World Outliner's reparent drag. Carries a fixed 4-byte scene::Entity rather than a
// string, so every target guards on size EQUALITY -- kAssetDragDropType's payload is a path of
// unknown length and cannot.
static constexpr const char* kOutlinerReparentDragDropType = "AVER_OUTLINER_ENTITY";

// Dragging assets BETWEEN Content Browser folders, which is a different question from dragging one
// into the level.
//
// A SECOND TYPE, NOT A REUSE OF kAssetDragDropType, and the difference is what the payload may
// contain. That one exists so the VIEWPORT never receives something it cannot place, so it is
// filtered to .ocmesh/.ocparticle and drops folders on the floor. Moving files has no such
// restriction -- somebody reorganising a project moves materials, textures, scripts, graphs and
// whole folders -- so reusing it would silently move a SUBSET of what was selected and leave the
// rest behind, which is the worst available outcome for a file operation.
//
// Both payloads are set from the SAME drag: ImGui allows several types per source, so one drag can
// be placeable in the viewport and movable in the browser at once, each target taking only the
// type it understands.
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
// THE canonical Aver orange -- was GraphEditor.cpp's IM_COL32(242,101,34) but had drifted into four
// different oranges: this theme's accent (0.95/0.42/0.13, six units off on green), the viewport
// selection outline's brighter 1.0/0.62/0.12, and an ad-hoc 0.79/0.47/0.16 on one button. Selection
// outline stays deliberately distinct (must read against arbitrary scene colour, not this chrome);
// the rest are unified here.
static constexpr ImVec4 kAverOrange   (242.0f/255.0f, 101.0f/255.0f, 34.0f/255.0f, 1.00f);
static constexpr ImVec4 kAverOrangeDim(242.0f/255.0f, 101.0f/255.0f, 34.0f/255.0f, 0.55f);

// Applies the editor's dark ImGui colour scheme and metrics.
// EVERY colour is set deliberately -- it used to leave most of ImGui's ~50 entries at the library
// default (blue), so tabs, scrollbars and docking read default-blue against an orange-on-steel
// identity. Neutrals carry a slight blue bias so the warm accent reads as chosen, not merely present.
// SPLIT FROM THE COLOURS ON PURPOSE, and the reason is a trap rather than tidiness.
//
// A runtime theme switch wants to rewrite the palette and nothing else. Re-running the whole style
// setup would look like the obvious way to do that, and it is wrong: the only caller is applyDpi,
// which runs ScaleAllSizes(dpi) immediately AFTER it, so calling the combined function on its own
// resets every metric to unscaled and silently drops the DPI scale. On a 1.5x display that takes
// FramePadding.y from 6 back to 4, which changes the docked tab-bar height, which moves the Level
// viewport rect -- and --probe-rel resolves against that rect, so all twenty oracle gates move at
// once. Metrics are applied with the DPI pass; colours can be applied any time.
static inline void applyEditorMetrics() {
    ImGuiStyle& s = ImGui::GetStyle();

    // Set UNSCALED -- applyDpi calls ScaleAllSizes(dpi) immediately after this, so writing
    // pre-multiplied values here would square the scaling on a high-DPI display.
    s.WindowRounding    = 4;  s.ChildRounding  = 4;  s.FrameRounding  = 4;
    s.PopupRounding     = 4;  s.GrabRounding   = 3;  s.TabRounding    = 4;
    s.ScrollbarRounding = 4;
    s.WindowBorderSize  = 1;  s.FrameBorderSize = 0; s.PopupBorderSize = 1;
    s.ChildBorderSize   = 1;
    s.WindowPadding     = ImVec2(10, 8);
    // FramePadding.y stays at 4 -- a gates constraint, not taste. A docked tab bar is FontSize +
    // FramePadding.y*2 tall; raising it shortens the Level viewport, and --probe-rel (relative to
    // vpX_/vpY_/vpW_/vpH_) survives a resize but not a change of aspect. The x half is free.
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

    // One ladder of neutrals, deepest to lightest, so depth is expressed by ONE consistent set
    // rather than by each widget family inventing its own near-black.
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

    // THE ACTIVE TAB is the single most-looked-at widget in a docked editor, and it was the ImGui
    // default blue. An orange top edge on a panel-coloured body reads as "this one", which is the
    // job; a fully orange tab would shout.
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
    // Which typed glyph draws this entry when no sprite art exists for it. -1 = none, falling back to
    // the anonymous page. Resolved at LISTING time beside `tile`, not at draw time: the gallery redraws
    // every frame, and an extension lookup per tile per frame is what the 20-frame cache avoids.
    int kind = -1;
    std::string kindExt;   // lower-cased extension, for assetKindFor at draw time
    // ONLY MEANINGFUL when kindExt == ".ocgraph". Read from the file's DOMAIN record at LISTING
    // time, same as `kind`/`kindExt` above, and for the same reason: this view redraws every frame,
    // and re-parsing a graph's header per tile per frame is the cost the 20-frame listing cache
    // exists to avoid. Defaults to Gameplay, which is also what an absent DOMAIN record means, so a
    // non-graph entry (which never reads this field) and a graph this build failed to open both read
    // the same, harmless way.
    editor::GraphAssetFamily graphFamily = editor::GraphAssetFamily::Gameplay;
};

// A Content Browser directory listing, refreshed on a frame stamp. Folders sort first and are counted.
struct DirListing { int stamp = -1000; std::vector<DirEntry> entries; usize dirCount = 0; };

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

// Converts a glTF/GLB into one .ocmesh per mesh (+ .ocskel/.ocanim if skinned) into destDir.
// Free function, not a member: two callers share no SandboxApp -- Content Browser Import
// (SandboxApp::importModel) and --import-gltf (createApplication, before any SandboxApp exists). The
// loop is pure modules/formats calls.
// Returns false with *outWhy only on a hard parse failure; a clean parse writing nothing new returns
// true with an all-zero summary -- callers decide if that counts as failure.
//
// contentDir: the project's Content directory, where the glTF's materials and textures are cooked to
// (empty = geometry only). Defined in SandboxContentBrowser.cpp.
// overwrite: replace outputs that already exist (each goes to the recycle bin first) instead of
// skipping them.
bool importGltfToDir(const std::string& src, const std::string& destDir, const std::string& contentDir,
                     bool overwrite, GltfImportSummary& out, std::string* outWhy);

// Forward-declared so SandboxApp::handleOpenRequest (single-instance forwarding's accept/decline
// check) can call them; full definitions stay in their natural home above createApplication.
bool isLevelFile(const char* p);
std::string ownerProjectOf(const std::string& mapPath);

// G-BUFFER DEBUG VIEW.
// WHY: gBufferVelocityTexture/gBufferViewZTexture/gBufferNormalRoughnessTexture (RHI.hpp) are SRVs
// nothing consumes yet -- a buffer nobody samples looks identical whether correct or silently all
// zero (see MEMORY "Unbacked verification claims").
// WHY A NEW IRenderFeature: the G-buffer is generic IDevice state owned by no render-feature module,
// so IRenderFeature::overlayPass (RHIResources.hpp) is the seam built for exactly this.
// WHY overlayPass: it runs after the post chain resolves MSAA/tonemap and after the deferred sky, so
// this view can't be silently overwritten by the sky's depth-EQUAL fill.
// SCOPE: output-only, never writes the three textures. Forward-declared below (long raw string reads
// better out of the way) but init() calls it.
static const char* gbufferDebugShaderSource();

class GBufferDebugFeature final : public rhi::IRenderFeature {
public:
    enum class Mode : u32 { Off = 0, Velocity = 1, ViewZ = 2, NormalRoughness = 3 };

    // Texels/frame that saturate the debug pixel. Chosen, not measured: 8 texels/frame is already a
    // brisk pan at edit-viewport res, so "fully saturated" must read as faster than this scale shows --
    // why the shader biases by 0.5 instead of a bare multiply.
    static constexpr f32 kVelocityFullScaleTexels = 8.0f;
    // World-space distance (centimetres, this engine's convention) mapped to fully white in the
    // view-depth debug view. Cosmetic only -- gBufferViewZTexture's real values are untouched;
    // changing this affects only what this view draws.
    static constexpr f32 kViewZDebugFarUnits = 5000.0f;

    const char* name() const override { return "GBufferDebug"; }

    // Borrowed; the device outlives this feature for the run (same non-ownership every other
    // IRenderFeature in this file already assumes -- see voxiRenderer_'s own member comment).
    void setDevice(rhi::IDevice* dev) { device_ = dev; }
    void setMode(Mode m) { mode_ = m; }
    // The 3D viewport's rect within the backbuffer (vpX_/vpY_/vpW_/vpH_), NOT the whole window:
    // overlayPass hands this feature a full-backbuffer viewport/scissor, so without narrowing it a
    // fullscreen triangle would paint over the editor chrome too. Zero means "skip this frame".
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

        // x: mode; y: velocity full-scale in texels/frame; z: viewZ debug far distance; w: unused.
        // Kept in sync BY HAND with gbufferDebugShaderSource()'s own PSGBufferDebug -- this string
        // has no access to the Mode enum above, see that shader's own top comment.
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

    // Releases the pipeline/binding set while the device is still alive. Idempotent. The destructor
    // calling this is a fallback: it runs after onShutdown, when the resource factory is gone and
    // `res_` dangles -- see onShutdown, where both are torn down explicitly and in order.
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

    // Lazy, like FxaaResolve::ensurePipeline -- the same standalone fullscreen-triangle recipe,
    // against three G-buffer SRVs instead of one scene colour, and against the backbuffer format
    // directly rather than a caller-supplied outTarget (the backbuffer is already bound).
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
            // RGBA8Unorm, NOT device_->backbufferFormat() -- that method's NAME is the trap: it returns
            // kSceneColorFormat (RGBA16F, the HDR *scene* target), not what overlayPass draws onto.
            // Trusting it once produced 12 debug-layer errors/frame then device removal:
            //   "The render target format in slot 0 does not match ... (pipeline state =
            //    R16G16B16A16_FLOAT, render target format = R8G8B8A8_UNORM, RTV = 'Viewport.Composite')"
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

    // Own root CBV register: this shader never includes rhi::sharedShaderPrelude(), so b1 here is
    // unrelated to what b1 means to a pipeline that does -- the identical reasoning
    // kFxaaConstantRegister's own comment gives (modules/render.sr/src/AverSrFxaa.cpp).
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

// The HLSL behind GBufferDebugFeature -- a standalone fullscreen-triangle pass with its own root
// signature (no rhi::sharedShaderPrelude(), the same choice AverSrFxaa/AverSrSpatial make for the
// identical reason: it reads nothing any OTHER pipeline's per-frame/per-draw constants declare).
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
    // ---- particles DECIDED 4: the two halves of particles::ParticleRenderer::GiSeam ----
    // Both static (like resolveMaterialTexture/depthProxyLookup nearby): only read voxiRenderer_'s
    // members, through `user`. modules/particles never sees this file or the fact that "voxi" is on
    // the other end of its GiSeam -- these two functions are that entire boundary.

    static bool particleGiPrepare(u32 srvBase, u32 samplerBase, u32 cbRegister,
                                  std::string* outPrelude, std::string* outDefines, void* user);

    static void particleGiBind(rhi::IResourceFactory& res, rhi::BindingSetHandle set, u32 srvBase,
                               const void** outCbData, u32* outCbBytes, void* user);
#endif  // AVER_MODULE_VOXI && AVER_MODULE_PARTICLES && AVER_MODULE_SCENE

    // ---- content and mesh loading ----
    // The content index, the asset resolvers, the mesh registry and the material cache are content_
    // (aver::game::GameContent), the same component the standalone runtime uses. What stays here is
    // what only the editor builds on top: pick triangles, triangle counts, the LOD ladder and cluster
    // data (see onMeshLoaded), the eager material preload for the picker, and the reload wrappers.

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

    // Loads every .ocmat under Binaries/Materials AND Content/Materials and binds each to the
    // surface token its stem interns to -- the SAME two homes (and the SAME precedence) the
    // per-name resolver materialForSurface() already honours. A name is gathered once even when
    // it exists under both: materialForSurface() itself tries binariesDir first, so the built one
    // under Binaries wins the collision, matching the runtime's documented order.
    // NON-RECURSIVE in each directory, matching materialForSurface(), which only ever looks directly
    // inside those two folders.
#if AVER_MODULE_PBR
    void loadProjectMaterials();

    void releaseProjectMaterials();
#endif


    void makeMaterialFor(MeshObj& o);

    void onRender(Engine& e) override;

    void skinDrawCheck(Engine& e);

    void submitGameUi(Engine& e);

    void drawUiDemo();

    int exitCode() const override;

    void onShutdown(Engine& e) override;
    void setVSyncOff(bool off);               // --no-vsync
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
    // --drone-graph <relPath>. `droneGraphRel_` itself is guarded `#if AVER_MODULE_SCENE` at its
    // declaration, so this setter must be guarded too -- SAME PRE-EXISTING SCOPING BUG as
    // setFogMatchToStreamRadius below, fixed while verifying this task's own -DAVER_MODULE_SCENE=OFF
    // build. Not part of the LOD-select/mesh-cluster work; flagged separately in this task's report.
#if AVER_MODULE_SCENE
    void setDroneGraph(std::string relPath);
#else
    void setDroneGraph(std::string);
#endif

    void setLandscapePath(std::string path);

    // --fog-match. A negative opacity means "leave the target where it is" and just switch matching
    // on. PRE-EXISTING SCOPING BUG, fixed in passing while verifying THIS task's own
    // -DAVER_MODULE_SCENE=OFF build: this setter referenced matchFogToStreamRadius_/
    // fogMatchTargetOpacity_, which ARE guarded #if AVER_MODULE_SCENE at their declaration -- unlike
    // setChunkStreamAuto/setDroneAuto above, so it simply failed to compile with the module off.
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
    void setGiHistoryResetAt(int framesIn);
#if AVER_MODULE_SR
    void setAverSrCycleAuto(int framesIn);   // --aversr-cycle [N]
#endif
    void setResizeCycle(int n);              // --resize-cycle [N]
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
#if AVER_MODULE_SYNAPSE
    void setBakeNavOnStart(f32 cellCm);   // --bake-nav [cm]

    // Rebuilds the overlay line mesh from nav_. Destroying the old one FIRST is the point: this
    // runs on every bake and every level open, and before destroyLineMesh existed each call
    // leaked one committed upload buffer for as long as the editor stayed open.
#if AVER_MODULE_PHYSICS
    void rebuildColliderOverlay(Engine& e);
#endif

    void buildReferencesPanel();

    void buildProfilerPanel(Engine& e);

    void rebuildNavOverlay(Engine& e);

    bool bakeNavigationNow(Engine& e, std::string* why = nullptr);

    void loadNavForLevel(Engine& e);
#endif
    void setFocusCompile(bool b);   // --compile-scripts
    void setFocusReload(int frames); // --reload-scripts [N]
    void warnDeadMaterialHandle(i32 mat);

    // Bounds a caller-supplied PlannedDraw buffer -- see planEntityDraws' own "Capacity truncation"
    // test case (SceneSubmissionTest.cpp T1). No content in PTTest or JungleRuins comes close (the
    // plan's own MADR/MHDR parse found 3-7 parts per multi-material tree, the deepest split seen);
    // 64 is a wide margin over that, not a tuned minimum.
    static constexpr u32 kMaxPlannedDraws = 64;

    // F2: THE ONE RESOLVER. Every one of the three copies this closes (the entity loop's own steps,
    // formerly 5862-5935; the deleted submitShadowOnly lambda's, formerly 5546-5605; drawMeshParts'
    // own, formerly 8299-8330 -- drawMeshParts itself is gone too, subsumed by planEntityDraws +
    // emitEntityDraws below) ran surfaceMaterials_.find, MaterialLibrary::desc, isTranslucent,
    // surfaceLooks_.find and the MaterialSystem lookup BY HAND, and the third of those had already
    // drifted from the other two -- it never checked liveness, so a dead handle there baked in the
    // bright-white-mirror identity as final (see resolveSurfaceLook's own comment in
    // SceneSubmission.hpp for why that is one of the worst possible failure appearances).
    //
    // `mat` is the material TOKEN, already carrying whatever entity- or part-level
    // meshDefaultMaterial/part-slot fallback the caller resolved -- this never re-derives that
    // fallback itself, only what the token resolves to.
    struct ResolvedSurface {
        aver::editor::SurfaceLook look;
        u32 authored = 0;                    // pbr::MaterialLibrary handle, or 0 (built-in look/fallback)
        rhi::BindingSetHandle matSet = 0;
        const void* matConstants = nullptr;   // a reference into MaterialSystem's own storage (5931's contract)
        u32 matBytes = 0;
    };

    ResolvedSurface resolveSurface(i32 mat);

    void emitEntityDraws(Engine& e, const aver::editor::PlannedDraw* draws, u32 n, const Mat4& wm,
                        const aver::editor::RouteDecision& route, bool prepassEligibleBase);

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
    void setPtBounces(int n);                        // --pt-bounces N
    void setLayeredBsdf(int n);                    // --layered-bsdf N
    void setCoat(f32 w, f32 r, f32 f0);   // --coat W [R] [F0]
    void setGiUpdateInterval(int n);          // --gi-update-interval N
    void setGiMode(int n);                             // --gi-mode N
    void setRestirVisibility(int n);
    void setDenoiser(int n);                          // --denoiser 0|1
    void setReblurAccum(int n);                    // --reblur-accum N
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

    const char* averSrSourceText(voxi::AverSrSource source) const;

    const char* averSrAutoRungName(const voxi::Settings& s, const voxi::DeviceInfo& d) const;

    void updateAverSrAuto(Engine& e);
#endif

    void syncPtSceneView(rhi::IDevice* dev);

    void setFrameTimeReport(bool on);                 // --frame-time
    void setRayDrivenAblation(int m);
    void setRtDenoiseMotionTaper(f32 v);
    void setRefractionOverrides(int mode, f32 strength, f32 fade);
    void setMsOverride(bool on);                           // --ms
    void setProbe(u32 x, u32 y);                    // --probe X Y
    void setProbeRel(f32 u, f32 v);                 // --probe-rel U V
    void setCamWobble(f32 degrees, i32 periodFrames);
    void setMeshHeapDefault(bool on);
    void setLodShareVertices(bool on);
    void setCamTranslate(f32 speedCmPerFrame);
    void setCamera(Vec3 pos, f32 pitchDeg, f32 yawDeg);
    void setScriptsDir(std::string d);            // --scripts <dir>
    void setSpawnTest(std::string cls);      // --spawn-test <ClassName>
    void setUnlitMode(bool on);                                  // --unlit
    void setPlayTest();                                       // --play-test
    void setProjectPath(std::string p);          // <path>.ocproject
    void setStartMode(std::string m);
    void setOpenMap(std::string p);
    void armBrowser(bool on);   // shows the start screen
    void setSingleInstanceEligible(bool b);

private:
    // Adopts the project the browser or command line loaded, and refreshes everything keyed to it.
    // Opening a project is the longest blocking thing the editor does after startup -- reloads every
    // material/mesh, cooks LOD pipelines, loads the start map, starts the script host, all on the main
    // thread between two frames -- so the window stopped painting and Windows greyed it out as "Not
    // Responding" with nothing to say why.
    // THE SPLASH ALREADY SOLVED THIS ONCE, for engine startup ("naming the current stage turns 'it
    // froze' into 'it is compiling shaders'"). Same window, same status line, reused one layer up.
    // Scoped to the load: appears when one starts, destroyed when it ends, no early-return leak.
    struct LoadingScreen {
        Splash splash;
        bool on = false;
        Engine* borrowed = nullptr;   // non-null when reusing the engine's startup splash

        // `eng` is borrowed rather than owned when its startup splash is STILL UP: opening a project
        // from the command line lands inside onInit, where that splash is showing; opening one from
        // the browser lands frames later, where it's long gone. This distinguishes a second top-most window from the first.
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
        ~LoadingScreen() {
            // No minimum visible time: a project that loads instantly should not look like it didn't.
            // Startup uses one because a flashing splash reads as a glitch; here the main window is
            // already up behind it. A borrowed splash belongs to Engine::run, which closes it when startup finishes.
            if (on) splash.close(0);
        }
    };

    void applyProject(Engine& e);

#if AVER_MODULE_LANDSCAPE
    void maybeAutosavePrefs(f32 dt);

    // ONE MESH PER MATERIAL: content_ splits a mesh naming several materials at load
    // (GameContent::buildMeshParts, partsFor) -- split rather than drawn as ranges because the ray
    // path's BLAS carries one material per instance.
    //
    // drawMeshParts is GONE. F4 (occlusion-fix-plan.md) folded its whole job -- "a mesh that names
    // several materials draws as several meshes, one per slot" -- into planEntityDraws()
    // (SceneSubmission.hpp) plus emitEntityDraws() above, which both the raster and the direct route
    // now call the SAME way, so the split can never drift between them the way this function's own
    // copy of the resolution rule once had (it never checked material-handle liveness, unlike the
    // entity loop's copy -- see resolveSurface's own comment for the bright-white-mirror failure that
    // gap could have produced). Its lone call site (formerly 6360-6363) is replaced accordingly.

    rhi::LineHandle selectionOutlineLines(Engine& e, u64 meshId);

    std::string autosavePathFor(const std::string& levelPath) const;

    void maybeAutosave(f32 dt);

    void autosaveCancelNotice();

    void autosaveRunSave();

    void clearAutosave();

    void checkForRecovery();

    void drawRecoveryPrompt(Engine& e);

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

    // SAVED ON EDIT, NOT ON A BUTTON. The footer below argues for a button on the grounds that a
    // .ocproject is source-controlled and writing it on every slider drag makes noise nobody asked
    // for. That argument is real, and it lost to a simpler one: a settings page that requires a
    // separate click to mean anything will be edited and closed, and the edit will be gone. Twice
    // reported here. The button stays for anyone who wants it; it is now a confirmation rather than
    // the only way through.
    //
    // HALF A SECOND, twice the preferences debounce, because this file is bigger, is read by other
    // people, and is rewritten whole (writeOcproject preserves comments and unknown keys, so every
    // save re-serialises the document). A drag settles before it is written.
    static constexpr f32 kProjectAutosaveSec = 0.5f;
    f32 projectAutosaveAccum_ = 0.0f;


    // ---- FRAME BUDGET -------------------------------------------------------------------------
    //
    // Scales GI work to hit a target frame time. Off unless RENDER.FRAMEBUDGETMS is set, because a
    // renderer that silently changes its own quality is a renderer whose measurements cannot be
    // compared, and this repo takes a lot of measurements.
    //
    // WHY A BUDGET AND NOT A "CAMERA IS MOVING" TEST. Motion is the trigger people notice, but it is
    // not the thing that hurts: a still camera in a heavier scene misses the frame just as badly,
    // and a moving camera in a trivial one does not need help. Budgeting the frame covers both with
    // one rule, and the rule is stated in the unit anyone actually cares about.
    //
    // WHAT IT SCALES, AND WHAT IT DELIBERATELY DOES NOT. GI update interval first, then cone count.
    // Both were MEASURED on Sponza under --cam-wobble: raising the interval to 4 alone took GPU
    // total 51.1 ms -> 29.3 ms, which is nearly the whole regression, and cone count 13 -> 5 was
    // worth ~8% of the primary pass. RENDER SCALE IS NOT ON THIS LADDER even though it is the
    // single biggest lever, because rebuilding the render targets from inside a frame has taken the
    // device down before (see the renderScale recovery path); AverSR is the supported way to trade
    // resolution and it is chosen deliberately, not by a controller.
    //
    // ONE STEP AT A TIME, WITH HYSTERESIS. Dropping quality is allowed to react quickly, raising it
    // is not: an unstable controller that oscillates between two rungs is more distracting than the
    // frame it was trying to save, because the eye tracks CHANGE in indirect light far better than
    // its absolute level.
    //
    // THE CONTROLLER ITSELF is voxi::frameBudgetTick (modules/render.voxi FrameBudget.hpp), shared with
    // the standalone runtime; frameBudget_ is its state.
    f32  frameBudgetMs_ = 0.0f;       // RENDER.FRAMEBUDGETMS; <= 0 disables the whole controller
    bool frameBudgetForced_ = false;  // --frame-budget: run the controller even in a capture
    voxi::FrameBudgetState frameBudget_;

    void frameBudgetTick(f32 dt, voxi::Settings& vs);

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

    // SAVE ALL: the level plus every dirty asset tab (AssetEditorHost::saveAllDirty, which until
    // 2026-09-16 ran only from the quit prompt). File > Save All and Ctrl+Shift+S. Defined in
    // SandboxShell.cpp.
    void saveAll();

    // VIEWPORT SCREENSHOT to <project>/Saved/Screenshots (File > Take Screenshot, F9). The writer used to
    // exist only inside captureCheck's --frames gate. request...() only sets the latch; service...()
    // runs once a frame beside captureCheck (SandboxRender.cpp), asks the device for a capture, and
    // writes the PNG -- cropped to the 3D viewport -- when the frame image arrives.
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
    bool pieCamLooked_ = false, pieCamKeptYaw_ = false, pieCamKeptPitch_ = false;
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
    // ---- MULTI-SELECTION -------------------------------------------------------------------------
    //
    // ANCHOR PLUS SET, and the anchor is the existing sel_/selEntity_ pair rather than a replacement
    // for it. Selection is read in dozens of places -- the Details panel, the gizmo, F-focus, the
    // outline, copy, rename, the status line -- and every one of them wants ONE entity to talk about.
    // Rewriting them all to ask "which of the several?" would be a much larger change for no gain,
    // so selEntity_ keeps meaning exactly what it meant and multiSel_ is the rest.
    //
    // THE INVARIANT, copied verbatim from the Content Browser's own multi-select (cbSelection_, which
    // already works and which this deliberately mirrors so the two behave the same under the same
    // keys): multiSel_ CONTAINS selEntity_ whenever anything is selected. A caller that iterates
    // multiSel_ therefore sees the whole selection including the anchor, and never has to remember to
    // add it back -- forgetting that is how "delete removed all but one" bugs happen.
    std::vector<scene::Entity> multiSel_;

    bool multiStale() const;
    bool multiIsSelected(scene::Entity e) const;
    void multiSyncToAnchor();
    void multiClear();
    void multiSetSingle(scene::Entity e);
    void multiToggle(scene::Entity e);
    void multiRange(scene::Entity to);
    std::vector<scene::Entity> selectedEntities() const;
    // The rows the outliner drew this frame, in draw order. Rebuilt every frame by drawOutlinerRow;
    // read only by multiRange.
    std::vector<scene::Entity> outlinerOrder_;
    std::string outlinerFilter_;   // name filter box; empty = show everything
    // Filled when the delete-confirm modal opens; see cbFindReferencesTo for what it can and
    // cannot see. Cleared on delete or cancel so a later modal never shows a previous answer.
    std::vector<std::string> cbDeleteRefs_;
    std::vector<std::string> cbRenameRefs_;   // the same, for the rename dialog
    // Whether the rename dialog will repoint what it found. ON by default: repointing is what an
    // author wants nearly every time, and the checkbox exists so a tool that edits other people's
    // files can be told not to.
    bool cbRenameRepoint_ = true;

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
    // entry at all for an add/delete (deleteSelection()'s objects_ branch).
    // asset/meshId/material, which this struct used to carry directly, are gone: describeEntity() now
    // captures every component via EntitySnapshot -- see EditorEntitySnapshot.hpp for what it
    // deliberately omits (hierarchy; CName's internal blob offsets).
    struct EditCmd {
        enum class Kind { Transform, Create, Destroy, CreateObj, DestroyObj, LandscapeStroke, FoliageStroke, Reparent, Material, Rename, RemoveComponent, Visibility };
        Kind kind = Kind::Transform;
        // WHICH EDIT THIS IS, monotonically. Identifies the document's state so a save can record
        // "clean as of here" -- see levelHasUnsavedEdits. Never reused, so undo and redo move the
        // mark back and forth across a save point correctly.
        u64 serial = 0;
        EditId id = 0;            // a scene entity, through the indirection
        int objIndex = -1;        // or an objects_ index, for the placeholder scene
        EditXform before{}, after{};

        // EVERY OTHER ENTITY A MULTI-SELECTION MOVE TOOK WITH IT.
        //
        // THE BUG THIS EXISTS TO CLOSE, and it was silent scene corruption on an everyday gesture:
        // the gizmo's multi-move applied the anchor's world delta to every other selected entity
        // with a bare setLocalTransform and recorded nothing, while endTransformEdit pushed ONE
        // command keyed on selEntity_. Select twenty props, drag them across the level, press
        // Ctrl+Z -- the anchor snapped home and the other nineteen stayed where they had been
        // dragged. Redo could not repair it either, because the redo had nothing to say about them.
        //
        // ONE COMMAND, NOT N. Delete takes the other road (one record per entity, so a five-object
        // delete needs five undos) and says so in its own comment. A move is different in kind: a
        // delete of five things is arguably five edits an author might want to unpick separately,
        // but a drag is ONE gesture and undoing it half-way leaves the scene in a state the author
        // never saw. Grouping them here is what makes Ctrl+Z mean "undo that drag".
        //
        // LOCAL TRANSFORMS, NOT WORLD. before/after above are world-space (selectedXform's own
        // convention) and get converted on the way back in. These are stored exactly as they will be
        // written, so undo cannot drift through a conversion -- and an entity whose PARENT is not in
        // the selection is restored to the local transform it actually had, whatever that parent was
        // doing at the time.
        struct AlsoMoved { EditId id = 0; Transform beforeLocal, afterLocal; };
        std::vector<AlsoMoved> alsoMoved;

        // VISIBILITY payload: every selected entity's AUTHORED-visible flag, before and after one
        // click on the Details panel's Visible checkbox -- the identical "one gesture, N entities,
        // a before/after pair per entity" shape AlsoMoved just above already uses for a multi-
        // selection drag, reused here rather than invented fresh. `before`/`after` are what
        // authoredVisible() returned for that entity; undo/redo replay them through
        // setAuthoredVisible(), which also drops the entity from editorHidden_ -- so undoing a
        // Visible edit on something that happened to be H-hidden at the time makes it visible
        // again rather than quietly reinstating a session-only hide the checkbox never promised
        // to preserve.
        struct VisibilityChange { EditId id = 0; bool before = true; bool after = true; };
        std::vector<VisibilityChange> visibility;
        std::string label;        // outliner display name; editor-owned bookkeeping, not World's
#if AVER_MODULE_SCENE
        editor::EntitySnapshot snap;    // scene entity: asset name, persisted id, every other component
#endif
        bool hadBody = false;
        Vec3 bodyHalf{0,0,0};
        // THE TWO AUTHORING FLAGS THAT ARE NOT COMPONENTS, and so are not in `snap`.
        //
        // nocollide and snapToGround are load-time instructions carried by the .ocworld PLACE record
        // and held afterwards only in entityCollide_/entitySnapZ_ -- there is no component for
        // captureEntity to find. destroyEntity erases both maps, and nothing put them back, so
        // deleting a walk-through prop and pressing Ctrl+Z restored the entity while silently
        // dropping its nocollide. The next save then wrote collide=true, and the next LOAD gave it a
        // static body it never had: a decoration you could walk through became solid, one undo and
        // one save later, with nothing logged.
        bool  hadCollide = true;      // the default saveLevel writes for an entity it has no entry for
        bool  hadSnapZ   = false;
        f32   snapZ      = 0.0f;
        MeshObj objSnapshot{};    // CreateObj/DestroyObj payload; MeshObj is trivially copyable
#if AVER_MODULE_PBR
        // Material payload. THE WHOLE DESC, BOTH SIDES, not the one slider that moved: a MaterialDesc
        // is a few hundred bytes and the panel's controls interact (ior against reflectance, alpha
        // mode against transmission), so replaying "roughness was 0.4" would restore a state that
        // never existed if two knobs moved in one interaction. Undo depth is 64, which bounds it.
        pbr::MaterialHandle matHandle = 0;
        pbr::MaterialDesc matBefore{}, matAfter{};
#endif
        // Rename payload. `label` above already carries the NEW name for a Create; these two are the
        // pair a Rename swaps between, and they are the outliner label rather than CName -- see
        // applyEntityLabel for why the editor's display name is the one being edited.
        std::string renameBefore, renameAfter;

#if AVER_MODULE_SCENE
        // RemoveComponent payload: the removed component's type and byte-exact contents, captured
        // the moment before removal so undo can put it back. Reuses EntitySnapshot::Comp -- the
        // identical "one component, byte copy" shape captureEntity's own loop already produces for
        // `snap` above -- rather than a second version of the same three lines.
        editor::EntitySnapshot::Comp removedComponent;
#endif

        // LandscapeStroke payload: the heightfield sub-rectangle a brush stroke touched, before and
        // after.
        // A RECT DIFF, NOT A SECTION SNAPSHOT: a 512x512 section is a megabyte of floats, so two
        // copies per stroke would put a hundred megabytes on the undo stack in a minute of painting; a
        // touched rect is usually kilobytes.
        // ONE ENTRY PER STROKE, not per frame: the rect is UNIONED across the stroke and pushed once on
        // release, or a two-second drag would take sixty Ctrl+Z presses to undo.
        u32 landX0 = 0, landY0 = 0, landX1 = 0, landY1 = 0;   // inclusive sample bounds
        std::vector<f32> landBefore, landAfter;

        // FoliageStroke payload: every entity one brush stroke created or erased, as one entry.
        // ONE ENTRY PER STROKE for the same reason LandscapeStroke is: a foliage brush places dozens
        // of instances per second, and a Create per instance would mean a two-second drag cost a
        // hundred Ctrl+Z presses. The snapshots let recreateFrom() undo an erase by putting the same entities back.
#if AVER_MODULE_SCENE
        std::vector<editor::EntitySnapshot> batchSnaps;
        std::vector<EditId>                 batchIds;
#endif
        bool batchWasErase = false;   // which direction undo has to run

#if AVER_MODULE_SCENE
        // Destroy's DESCENDANTS, parents before children, excluding the root the command already
        // names through `id`/`snap`.
        //
        // A DELETE TAKES A SUBTREE AND AN UNDO HAS TO PUT ONE BACK. World::destroy retires the whole
        // subtree, so a Destroy carrying one snapshot could only ever restore one entity: parent a
        // lamp to a table, delete the table, Ctrl+Z, and the table returns alone with the lamp gone
        // for good. That is data loss, not a missing nicety, and it becomes reachable the moment a
        // level file can express a hierarchy.
        //
        // `parent` indexes THIS vector, with -1 meaning the command's own root -- not an EditId,
        // because the whole subtree is destroyed and rebound in one go and an id would have to be
        // re-resolved mid-restore.
        struct DestroyedNode {
            EditId id = 0;
            i32 parent = -1;
            EditXform xf{};
            std::string label;
            editor::EntitySnapshot snap;
            bool hadBody = false;
            Vec3 bodyHalf{0,0,0};
            // See EditCmd's own copy above: not components, so not in snap.
            bool  hadCollide = true;
            bool  hadSnapZ   = false;
            f32   snapZ      = 0.0f;
        };
        std::vector<DestroyedNode> subtree;

        // THE DESTROYED ENTITY'S OWN PARENT, as an EditId; 0 means it was a root.
        //
        // `subtree` restores everything BELOW the entity the command names. Nothing recorded what
        // was ABOVE it, so undoing the deletion of a child put it back at the top level -- and,
        // because its transform is stored parent-relative, at that offset from the world origin
        // rather than from where its parent is. The subtree half of this shipped without the half
        // that keeps the deleted thing attached to what it hung from.
        EditId parentId = 0;

        // Reparent's before/after parent, as EditIds; 0 means root.
        //
        // SEPARATE FIELDS even though the shape matches parentId just above, because that field's
        // meaning is Destroy-specific -- "the destroyed entity's own parent" -- and one field
        // carrying two meanings across two Kinds is drift a later reader has no way to detect. The
        // before/after EditXform pair rides the existing `before`/`after` members.
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

    // RemoveComponent's two apply halves. restoreComponent is undo's (puts the captured bytes
    // back); removeComponentRaw is redo's, and also what the Details panel's own removal
    // (removeComponentFromSelection) calls to do the removal before pushing the undo entry.
    void restoreComponent(EditId id, const editor::EntitySnapshot::Comp& comp);
    void removeComponentRaw(EditId id, u32 type);

    // Removes one component from the selected entity as one undoable command -- the Details
    // panel's Remove Component handler. False when the entity is gone or does not carry `type`,
    // matching World::removeComponent's own refusal so a stale click is a silent no-op.
    bool removeComponentFromSelection(u32 type);

    // Visibility's apply: `undoing` picks which side of each EditCmd::VisibilityChange pair to
    // write, mirroring applyXformTo's own `undoing` parameter for the identical reason -- one
    // click on the Visible checkbox is one gesture across the whole selection, so Ctrl+Z must
    // undo all of it or none of it.
    void applyVisibilityTo(const EditCmd& c, bool undoing);
#endif

    void pushEdit(EditCmd c);

    // The set the multi-move loop will actually touch: selected, valid, not the anchor, and not
    // beneath another selected entity. THE SKIP RULE IS DUPLICATED FROM THAT LOOP ON PURPOSE and
    // must stay identical to it -- recording a before-state for an entity the mover skips would
    // restore something that never moved, and missing one the mover touches is the bug this whole
    // mechanism exists to close. Sharing one helper is what keeps the two in step.
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
                                   const std::string& label, bool hadBody, const Vec3& bodyHalf,
                                   bool restoreObjectId = true,
                                   scene::Entity parent = scene::kInvalidEntity,
                                   bool collide = true, bool hasSnapZ = false, f32 snapZ = 0.0f);

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
    // One entry per instance THIS SESSION'S BRUSH put down: its position (matched by erase) and the
    // collision radius it was placed with (0 for a type whose collisionRadiusCm is 0 -- see
    // foliagePlaceOne's own comment on why that must stay excluded from the interpenetration test on
    // BOTH sides, exactly like aver::world::ChunkGenerator.cpp's placedSolid).
    struct FoliagePlaced { Vec3 pos; f32 solidRadiusCm; };
    std::vector<FoliagePlaced> foliagePlaced_;
#endif
#endif

    void undo();

    void redo();

    std::string selectionLabel() const;

    f32 selectedRadius() const;

    // ---- PlayerStart: where the player spawns in ----
    // THE LEVEL FORMAT ALREADY HAD THE ANSWER AND NOBODY READ IT: OcWorldData's hasSpawn/spawnX/Y/Z/
    // Yaw were parsed and written but consulted nowhere -- not begin_play, not loadLevel, not
    // saveLevel. A level could state where the player starts and be ignored.
    // ONE RECORD, SO ONE MARKER: SPAWN is a scalar record, not a list, so PlayerStart must not become
    // a second, independent thing that can disagree with it. The marker is the editor's live handle
    // onto that record, the same shape hasLevelSun_/hasLevelFog_ have for SUN and FOG.
    // A TRANSIENT ENTITY, never pushed to levelEntities_, for the same reason the drone is not: it
    // must not also be saved as a PLACE record -- two sources of truth, one invisible.
#if AVER_MODULE_SCENE
    scene::Entity playerStart_ = scene::kInvalidEntity;

    // The Outliner row currently being renamed in place, and its edit buffer.
    // Create-a-landscape controls; the shape knobs are landscape_.noiseParams(), shared with the
    // ring generator so a created section and the tiles around it come from one set of numbers.
    // Autosave. TEN MINUTES, and it was thirty seconds until the countdown made that cadence
    // visible for the first time.
    //
    // The old value was chosen when autosave was silent, on the reasoning that "a crash costs a
    // gesture or two" and the write itself never shows. Both halves are still true. What changed is
    // that autosaveRunSave does NOT call markLevelSaved -- correctly, since a sidecar is not a real
    // save -- so the level stays dirty afterwards and the timer immediately restarts. With a
    // ten-second warning on a thirty-second period, that put a countdown on screen for a third of
    // every minute, forever, until the level was saved for real. A safety net nobody can ignore is
    // one they turn off.
    //
    // Ten minutes is the interval Unreal ships and for the same reason: it is long enough that the
    // warning is a rare event worth reading, and a crash still costs one stretch of work rather than
    // an afternoon. The warning window stays at ten seconds, which is now 1.7% of the period instead
    // of 33%.
    static constexpr f32 kAutosaveDefaultSec = 600.0f;
    f32 autosaveIntervalSec_ = kAutosaveDefaultSec;
    f32 autosaveAccum_ = 0.0f;

    // THE COUNTDOWN, AND WHY IT NEEDS THREE STATES RATHER THAN A BOOL.
    //
    // The save is synchronous: saveLevel walks the whole world and writes it from inside onUpdate.
    // Engine::frameStep runs onUpdate BEFORE onRender, so a "Saving..." notification raised and then
    // saved in the same tick has already been replaced by "Saved" before buildUI ever draws -- the
    // one message describing the stall the user is about to feel would never appear. Pending exists
    // to spend a whole presented frame saying it, and to do the write on the tick after.
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
    // Preferences are cheap to check and tiny to write, so this can be far tighter than the level
    // autosave above: two seconds is short enough that nothing a person adjusts is worth losing,
    // and a tick that changed nothing does no I/O at all.
    // WAS 2 SECONDS, AND THAT WAS TOO LONG TO BE BELIEVED. A preference changed and then not seen
    // in the file is indistinguishable from one that never saved, and the gap was wide enough to
    // lose a change to any abrupt exit inside it.
    //
    // NOT ZERO, which would be the literal reading of "save directly": a slider being dragged dirties
    // the store on every frame, and at zero that is a file write per frame. A quarter second reads as
    // instant to a person and collapses a one-second drag into four writes instead of sixty. The
    // write itself only happens when something actually CHANGED -- setPrefString compares before
    // dirtying and flushEditorPrefs early-outs when nothing is dirty -- so an idle editor still does
    // no I/O at all, however short this is.
    static constexpr f32 kPrefsAutosaveSec = 0.25f;
    f32 prefsAutosaveAccum_ = 0.0f;
    bool autosaveWritten_ = false;
    bool autosaveFailedWarned_ = false;
    // Set by a notification button and consumed by maybeAutosave on the next tick. Deferred rather
    // than acted on inline because the buttons are drawn from onRender, which runs AFTER onUpdate --
    // acting immediately would apply a postpone to a save that had already happened this frame.
    bool autosavePostponeRequested_ = false;
    bool autosaveRetryRequested_ = false;
    std::string recoveryPath_;      // a sidecar newer than its level, waiting to be offered

    int landCreateSamples_ = 513;
    f32 landCreateSpacingCm_ = 100.0f;

    // --no-editor-chrome: suppress everything the editor draws ON TOP of the scene, so a capture
    // can be compared against AverEngineRuntime.exe's. Run-scoped and never persisted -- see the flag's own
    // comment in the argv loop for why it is not routed through showGrid_.
    bool noEditorChrome_ = false;

    // --scene-census, and the latch that makes it fire exactly once. Emitted from onUpdate rather
    // than from the load, because a project's class placements are spawned by a LATER stage than
    // loadLevel and a census taken at load would miss every one of them -- which is precisely the
    // class of divergence this is here to catch.
    bool sceneCensus_ = false;
    bool sceneCensusDone_ = false;

    scene::Entity outlinerRenaming_ = scene::kInvalidEntity;
    bool outlinerRenameFocus_ = false;
    char outlinerRenameBuf_[128] = {0};

    void beginOutlinerRename(scene::Entity e);

    void refreshPlayerStart();
#endif
    f32 playerStartYaw_ = 0.0f;

    bool playerStartTransform(Vec3& outPos, f32& outYawDeg) const;

#if AVER_MODULE_SCENE
    scene::Entity makePlayerStart(const Vec3& at, f32 yawDeg);
#endif

    void addPlayerStart(Engine&);

    void spawnPrimitive(Engine& engine, const char* assetPath, const char* label);

    void spawnCube(Engine& engine);

#if AVER_WITH_IMGUI
// DRAG-AND-DROP FROM THE CONTENT BROWSER, hence UI-only: spawnFromAssetDrop's sole caller is the
// viewport's ImGui drop target, and dropWorldPoint exists only to serve it. Both lean on
// viewportRay/lowerExt, which live in the browser half of this file.
#if AVER_MODULE_SCENE
    Vec3 dropWorldPoint(f32 screenX, f32 screenY, bool* onSurface = nullptr) const;


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
    // ---- VIEWPORT PLACEMENT VERBS (2026-09-16), dispatched from handleManip's edit-verb block and
    // defined in SandboxViewport.cpp. Each acts on the whole multi-selection as ONE undo entry.
    // End: drop each selected entity onto whatever is below it (rayPickGeometry + dropRestLift).
    void snapSelectionToFloor();
    // Arrow keys / PageUp / PageDown: move by the move-snap step (or 10 cm with snapping off).
    void nudgeSelection(const Vec3& deltaCm);

    // AUTHORED visibility: what the Details panel's Visible checkbox shows and what saveLevel
    // writes to the level (OcWorldPlacement::visible), as distinct from H/Shift+H/Ctrl+H's
    // SESSION-ONLY hide just below. True when kMeshRendererVisible is set OR the entity is in
    // editorHidden_ -- an entity H hid is still authored visible, so it saves, and reopens, that way.
    bool authoredVisible(scene::Entity e) const;
    // Sets the bit directly and drops `e` from editorHidden_: an authored edit supersedes whatever
    // temporary H-hide state the entity was in, so the bit alone is the truth again afterward.
    void setAuthoredVisible(scene::Entity e, bool v);

    // H hides the selection, Shift+H hides everything else, Ctrl+H brings back everything these hid.
    // SESSION-ONLY like every H verb here -- not because a level cannot store visibility (it can;
    // see authoredVisible/setAuthoredVisible just above) but because H is deliberately temporary,
    // the same role Unreal's own H plays beside a real, saved Visible checkbox.
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
    bool autosaveTestArm_ = false;   // --autosave-test: mark the level dirty once, then let it run
    bool autosaveTestLift_ = false;  // ...and lift the capture guard, loudly (see maybeAutosave)
    bool autosaveTestWarned_ = false;

    // --project-switch-test: opening a project that states NO render settings must not leave the
    // previous project's settings in force. Synthetic on purpose -- it drives applyProjectRenderSettings
    // with two hand-built ProjectDescs rather than two real .ocproject files on disk, because what is
    // under test is that function's guard, not the parser or the project browser. valid() needs only
    // a name and a dir, so no filesystem is touched and this runs headless on any machine.
    //
    // It falsifies a specific regression: projectBackend_ and frameBudgetMs_ used to be assigned
    // BELOW the hasRenderSettings() early return, so a bare project inherited both from whatever was
    // open before -- and the Rendering page then wrote the inherited backend into the bare project's
    // own manifest on the next unrelated edit.
    int projectSwitchFrames_ = 0;
    void runProjectSwitchTest();

    // --validate-graph <path>: run the managed graph validator over one .ocgraph and print the
    // answer. END-TO-END ON PURPOSE -- it goes C++ -> ScriptHost -> hostfxr -> HostBridge.GraphValidate
    // -> OcGraphParser/Graph.Validate and back with a real message. The editor-side plumbing has its
    // own headless test with a stub validator (GraphEditorLoadSaveTest), and a stub cannot prove the
    // export is reachable, that the buffer contract holds, or that the .NET runtime is even hosted.
    // Absence of the "exports no GraphValidate" warning at startup proves a symbol bound; this proves
    // it RUNS.
    // --graph-print-test: drives the on-screen graph-print feed's SINK, which is where its only
    // real logic lives. Not reachable from a headless suite: logSink is a static member of this
    // class installed into the core logger, and what it does depends on this instance's deque.
    //
    // It logs through AVER_INFO rather than poking graphPrints_ directly, so the prefix match is
    // part of what is under test -- a "[Graph] " that GraphInterop and this filter disagreed about
    // would write to the log and never reach the overlay, silently.
    int graphPrintTestFrames_ = 0;
    void runGraphPrintTest();

    // --clear-shader-cache: empty the DXIL blob cache and report what went.
    //
    // THE SECOND HALF OF THE UNBOUNDED-CACHE PROBLEM. A size bound stops it growing without limit
    // (D3D12Device::init sweeps to a budget at startup), but there was still no way for a person to
    // clear it deliberately -- and there are real reasons to want that: a DXC upgrade that emits
    // different DXIL for identical input is invisible to a content-keyed cache, and a machine short
    // on disk should not have to be told to go and find a hex-named directory under LOCALAPPDATA.
    //
    // A FLAG, NOT A MENU ITEM, for now: this is a maintenance action, not authoring, and a flag can
    // be run without opening the editor on a machine that is already short on space.
    int clearShaderCacheFrames_ = 0;
    // Which cache to clear. Empty means the real one under the user's data directory, which is what
    // a person running this wants. AN EXPLICIT PATH IS ACCEPTED because a command that deletes files
    // should be checkable end to end without deleting the ones you actually have -- verifying it by
    // running it against the developer's own 59 MB of blobs would cost them a recompile they never
    // asked for. It is also the form a CI workspace or a build server wants.
    std::string clearShaderCacheDir_;
    void runClearShaderCache();

    std::string validateGraphPath_;
    int validateGraphFrames_ = 0;
    void runValidateGraph();

    // --rename-repoint-test: the rename-repoint chain over REAL files in the real project.
    //
    // AssetRefScanTest covers the string surgery exhaustively and cannot cover any of this: which
    // files get opened, that the scan and the rewrite agree on the same set, that the write actually
    // lands on disk, and that a near-miss file sitting beside a real referrer is left byte-identical.
    // Those are the parts that edit somebody's project.
    //
    // Writes into a scratch folder UNDER the open project's content root, because that is the only
    // place cbFindReferencesTo will look, and removes it afterwards.
    int renameRepointFrames_ = 0;
    void runRenameRepointTest();

    std::string findRefsPath_;
    int findRefsFrames_ = 0;
    void runFindRefs();

    void runSaveDirtyTest();
    int saveDirtyTestFrames_ = 0;

    void runMultiSelectTest(Engine& eng);
    int multiSelTestFrames_ = 0;   // --multiselect-test: frames left before it fires

    // --asset-assign-test: the asset picker's three assignment helpers, headlessly.
    //
    // The PICKER itself is an ImGui popup and cannot be driven from here -- but nothing interesting
    // lives in it. What can go wrong lives in the assignment: which id space each field uses, whether
    // the render path is told to re-upload, and whether the level is marked dirty. Those are plain
    // C++ once extracted, which is why they were extracted.
    //
    // THE MATERIAL CASE IS THE POINT OF THIS TEST. A material is identified by an INTERNED NAME
    // TOKEN, while mesh and effect are both fnv1a64 of a project-relative path. Writing a path hash
    // into mr->material fails SILENTLY -- it resolves to no surface, or by coincidence to an
    // unrelated one -- so nothing would crash and the entity would just render wrong.
    // --graph-hits-test <graph.ocgraph>: the node-hit chain end to end, through the REAL bridge.
    //
    // The managed half has its own tests (NodeHitTests.cs: branch arms, diamonds, graph bleed). What
    // those cannot prove is anything on this side of the ABI -- that the two new exports actually
    // bind, that a graph name marshals across, that the "nodeId:age;..." payload survives the round
    // trip and parses back into pairs, and that disarming really stops it. A stub cannot fail those.
    //
    // Driven through graphLoad/graphTick, which host ONE graph on ONE entity with no class registry
    // and no Play state involved -- the smallest thing that makes real compiled IL execute.
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

    void buildUI(Engine& e);

#if AVER_WITH_IMGUI
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
    // Three formats can be created here and share everything except the bytes they write, so the
    // common half is these two helpers rather than a fourth copy of the same loop. Each "New X" item
    // is then: pick a free path, write, adopt.
    // NO NAME PROMPT, unlike New Folder: a new asset lands as New<Kind>.<ext> and is renamed with the browser's existing Rename, which every other asset already uses.

    std::filesystem::path cbFreeAssetPath(const char* stem, const char* ext);

    void cbAdoptNewAsset(const std::filesystem::path& target, bool openEditor = true);

    void cbCreateMaterial();

    void cbCreateSoundGraph();

    // Writes a starter .ocparticle -- a small warm ember burst -- and opens it. Same shape as
    // cbCreateSoundGraph immediately above, for the same reason its own comment gives: this is a
    // format with a working editor tab and an icon (see the Content Browser's extension table) that,
    // until now, nothing could BRING INTO EXISTENCE from inside the editor at all.
    //
    // Guarded: editor::pxStarterEffect comes from ParticleEditor.hpp and fmt::OcParticleExtras/
    // saveOcparticle from aver/formats/OcParticle.hpp, neither reachable with AVER_MODULE_PARTICLES
    // off -- see the #include guard near the top of this file.
#if AVER_MODULE_PARTICLES
    void cbCreateParticleEffect();
#endif

    void cbCreateFoliageType();

    void cbCreateNodeGraph();

    void cbCreateBehaviourTree();

    // Writes a starter .ocinput -- an Input Scheme -- and opens it. Same shape as
    // cbCreateFoliageType immediately above, for the same reason: a format with a working editor
    // tab that, until now, nothing could bring into existence from inside the editor at all.
    void cbCreateInputScheme();

    std::string cbImportBlockedReason(const std::string& dir) const;

    std::string cbMoveDragPayloadFor(const std::string& dragged) const;

    static bool cbIsUnder(const std::string& path, const std::string& dir);

    void cbFolderDropTarget(const std::string& folderPath);

    std::vector<std::string> cbPruneNested(const std::vector<std::string>& in) const;

    bool cbCopyEntryTo(const std::string& src, const std::string& destDir);

    bool cbMoveEntryTo(const std::string& src, const std::string& destDir);

    std::vector<std::string> cbFindReferencesTo(const std::string& absPath) const;

    // ---- REPOINTING WHAT REFERENCED AN ASSET, after it has been renamed -----------------------
    //
    // Rewrites `oldRel` to `newRel` in every text asset that anchored-matches it, in place. Returns
    // {files changed, references rewritten, files it could not write}.
    //
    // WHAT THIS DELIBERATELY CANNOT DO, because a feature that claims to have fixed everything is
    // worse than one that says what it missed:
    //
    //  - A reference stored as a HASH with no path text. A .ocmat TEX record may be written
    //    `{guid:0x...}`, and a save game bakes fnv1a64 ids into component fields with no path
    //    anywhere in the file. Nothing textual can find those, so nothing textual can fix them.
    //  - Anything outside the five text formats the scan reads. A .ocmesh's interned material slots,
    //    a .ocbt's string table and a C# file that builds a path in code are all real references and
    //    all invisible here.
    //  - A material named by FILENAME STEM rather than by path (materialForSurface probes three
    //    directories), which is a different identity model again.
    //
    // NOT TRANSACTIONAL, and it cannot cheaply be: this is N separate file writes. Every write goes
    // through writeFileTextAtomic so no INDIVIDUAL file is ever left torn, but a failure partway
    // leaves some files updated and some not. The count of failures is returned rather than
    // swallowed so the caller can say so out loud.
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

    // Returns an ASSET sheet tile for an engine asset extension, or -1 -- WHAT AN ASSET LOOKS LIKE
    // WHEN THERE IS NO ART FOR IT. Separate from the source-file sheet: different textures, different
    // provenance (branding/ASSETS.md).
    // Only four extensions have sprite-sheet tiles; everything else fell through to one identical grey
    // page, so a folder of twenty asset types read as twenty identical documents.
    // GLYPH PLUS COLOUR, not colour alone: colour separates types at a glance, the glyph says WHICH
    // type up close. Glyphs come from Material Icons already merged into the UI font -- no new
    // dependency, and they scale since they're text, not a fixed-size bitmap.
    // A REAL RENDERED THUMBNAIL WAS NOT POSSIBLE FOR MOST OF THIS BROWSER'S LIFE: the RHI had no
    // texture-to-texture copy or readback until this session, so frame K's pixels could not survive
    // frame K+1 reusing the target. copyTexture is the missing primitive; ThumbnailCache is its first
    // consumer, falling through to this glyph until a thumbnail exists.
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
    // ---- the asset picker ------------------------------------------------------------------
    //
    // WHY THERE WAS NONE. Assigning an asset to a component field meant dragging it out of the
    // Content Browser, so closing that drawer made reassignment impossible -- and only ONE field
    // (CParticleEmitter::effect) even had a drop target. CMeshRenderer::mesh printed a hex id and
    // offered nothing at all; CMeshRenderer::material offered nothing either. Worse,
    // isPlaceableAssetExt only lets .ocmesh and .ocparticle START a drag, so a material could not be
    // dragged even in principle. A button beside the field bypasses all of that.
    //
    // ONE GENERIC WIDGET over (label, id) candidates rather than a picker per field: the three
    // fields differ only in where their candidates come from and in what an id MEANS, and both of
    // those belong to the caller. Shaped after the graph editor's node palette -- search box focused
    // on open, case-insensitive filter, a capped list that SAYS it is capped -- so the two
    // searchable popups in this editor behave the same way.
    //
    // CANDIDATES ARE BUILT PER FRAME BY THE CALLER AND CONSUMED IN IT. They come from maps a project
    // reload clears (meshPathById_, surfaceMaterials_), so keeping them across frames would be
    // keeping a list of things that may no longer exist.
    struct AssetChoice { std::string label; u64 id = 0; };

    bool assetPicker(const char* popupId, const std::vector<AssetChoice>& candidates,
                     u64 current, u64* picked);
    char assetPickerFilter_[64] = {};

    // ---- the three assignments, each its own function because an "asset id" is three things -----
    //
    // NOT UNDOABLE, AND THAT IS A TESTED CONTRACT rather than an oversight: runSaveDirtyTest asserts
    // that a Details-panel asset write marks the level dirty through markLevelUnsaved() with no
    // EditCmd behind it. Giving these real undo would be a deliberate change to that contract, and
    // it is not a picker's business to make it.
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

    // Writes an edited material back to the .cs under Content\Materials that declares it, found by
    // trying each in turn. Returns the file written, or "" with err set.
    // THE GUARD IS ABOVE THE SIGNATURE, not inside the body, and it was inside: `pbr::MaterialDesc`
    // is in the parameter list, so with PBR off the function did not compile at all.
    // Content\Materials, NOT Binaries\Materials -- unlike loadProjectMaterials()'s READ, which
    // honours both, there is nothing to walk in Binaries here: it holds avermatc's compiled
    // .ocmat output, never the .cs source this function edits. A project whose materials were
    // moved to Binaries\Materials with no .cs left behind has nothing this function can write to,
    // which is what the error below now says.
#if AVER_MODULE_PBR
    std::string saveMaterialSource(const std::string& name, const pbr::MaterialDesc& d, std::string& err);
#endif  // AVER_MODULE_PBR

    void materialPanel(pbr::MaterialHandle handle);

    void buildModePanel(Engine& e);

    static bool editField(const char* label, std::string& value, usize cap);

    // panelFloat/panelInt MOVED to EditorWidgets.hpp (aver::editor namespace), with no behaviour
    // change, so any editor -- not just SandboxApp's own mode panels -- can avoid the narrow-dock
    // label-truncation bug they fix. Call sites below now say editor::panelFloat/editor::panelInt.

    void buildSelectModePanel();

    void buildSimulateModePanel();

#if AVER_MODULE_LANDSCAPE
    void buildWaterPanel(Engine& e);

    void buildLandscapeModePanel(Engine& e);

    void foliagePanelCreateType();

    void buildFoliageModePanel();
#endif

    void markLevelRecordEdited(bool& has);

    void buildPanels(Engine& e);

#if AVER_MODULE_SCENE
    // One Outliner row, resolved once per frame. `par` is the RAW engine parent; whether that
    // parent is itself listed is a separate question, decided in buildOutlinerPanel's second pass.
    struct OutlinerRow { scene::Entity ent; scene::Entity par; std::string shown; };

    static std::string lowerCopy(std::string v);

    std::string outlinerLabelFor(scene::Entity e) const;

    void drawOutlinerDragSource(scene::Entity ent);

    void drawOutlinerDropTarget(scene::Entity ent);

    void drawOutlinerRow(const OutlinerRow& row,
                         const std::unordered_map<u32, std::vector<const OutlinerRow*>>& children,
                         int depth);
#endif

    void buildOutlinerPanel();

    void buildDetailsPanel(Engine& e);

    void drawDrawer(Engine& e);

    void toggleDrawer(Drawer d);

    static ImVec4 notifyColour(editor::NotifySeverity s);

    void drawNotifications();

    void loadEditorPreferences();

    void resolvePreferredIdeFromPrefs();

    void saveEditorPreferences();

    void buildEditorPrefs();

    void buildWorldSettings();

    void buildProjectSettings();

#if AVER_MODULE_VOXI
    static void featureStatusBadge(aver::voxi::Renderer& vx, aver::voxi::Feature f);

    bool settingInt(const char* label, int* v, int lo, int hi, int whenEnabled, const char* tip);

    void buildWindowSettings();

    void buildImportSettings();

    void buildStreamSettings();

    void buildPhysicsSettings();

    void buildAudioSettings();

    void buildRenderingSettings(int page);
#endif

    void buildViewportOverlay();
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
    // MOVE, NOT SELECT, and the difference is whether a gizmo exists at all. Select draws none
    // (see drawGizmo's tool_ test), so an editor that opened in Select showed nothing to grab on a
    // freshly picked object and gave no hint that 2 would summon one -- "I cannot move things" is
    // the accurate description of that state, not a misunderstanding of it. Unreal likewise always
    // has a transform gizmo up on a selected actor. Select is still one keypress away on 1, and
    // --tool still overrides this.
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
    bool wantMeshReload_ = false;
    // Outliner display names. Not scene::World::name(), which holds the asset path.
    std::unordered_map<u32, std::string> entityLabels_;
    std::unordered_map<std::string, int> labelCounts_;
    std::unordered_map<u32, int32_t> entityBodies_;   // the static body an entity owns

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

    // Copy/Duplicate's source, and what Paste rebuilds from. `entities` and `hasObject` are set exclusively
    // of each other by copySelection() -- mirrors the existing loose pairing of sel_/selEntity_
    // rather than a variant type for two cases already mutually exclusive by construction.
    // ONE COPIED ENTITY. Split out of EditorClipboard so the clipboard can hold a LIST: Ctrl+C read
    // the anchor alone, so copying five selected props and pasting produced one -- the same
    // "applies to the set, acts on the anchor" shape that made multi-move unundoable and that Ctrl+D
    // was fixed for earlier this session.
    struct ClipboardEntity {
#if AVER_MODULE_SCENE
        editor::EntitySnapshot snap;
        // EVERYTHING UNDER IT, TOO. Copying only the entity the selection names meant pasting a
        // parent produced a childless copy -- silently, since the paste looked like it worked.
        std::vector<EditCmd::DestroyedNode> subtree;
#endif
        EditXform xform{};
        bool hadBody = false;
        Vec3 bodyHalf{0, 0, 0};
    };

    struct EditorClipboard {
        // Empty means nothing was copied -- this replaces the old hasScene bool outright.
        std::vector<ClipboardEntity> entities;

        // THE PLACEHOLDER PATH STAYS SINGLE-ITEM, deliberately. objects_/MeshObj is the no-project
        // path and has no multi-selection concept at all: multiSel_ is scene::Entity-typed and lives
        // behind AVER_MODULE_SCENE, so there is no set for it to copy. Turning this into a list too
        // would be inventing a feature nothing can reach.
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
    // A REFERENCE TO THE ONE REGISTRY, not an instance of its own. The asset-editor tabs reach the
    // same object through editor::keybinds(), and two registries would mean a rebind made on the
    // Preferences page silently failed to apply inside a graph or actor tab -- the exact "rebindable
    // unless you are in a tab" split this promotion exists to remove.
    editor::KeybindRegistry& keybinds_ = editor::keybinds();
#endif

    std::string makeEntityLabel(const std::string& surface, const std::string& asset);
    // The word makeEntityLabel numbers ("Wood" for M_Wood, else the asset's stem, else "Entity"). Split
    // out so saveLevel can tell a generated label from a renamed one without advancing labelCounts_.
    static std::string entityLabelBase(const std::string& surface, const std::string& asset);
    Tool tool_ = Tool::Move;   // see initialTool_ for why this is Move and not Select

    // Which mode the viewport is in, and the brush the Landscape mode is holding. Both members are
    // UNGUARDED even though sculpting is AVER_MODULE_LANDSCAPE-only: the mode switch, viewport hint
    // and input dispatch all read them from unguarded code. With the module off, Landscape simply never becomes reachable.
    EditorMode mode_ = EditorMode::Select;
#if AVER_MODULE_LANDSCAPE
    SculptTool sculptTool_ = SculptTool::Raise;
    // 1.0 is the smoothstep the brush always had; see BrushParams::falloff.
    f32 sculptFalloff_ = 1.0f;

    // FOLIAGE. One entry per .ocfoliage TYPE ASSET found in the project's content folder -- NOT one
    // per mesh, unlike before this format existed. What a species places, and how (mesh, material,
    // scale range, weight, randomizeYaw, collisionRadiusCm, alignToNormal), now lives in the loaded
    // fmt::OcFoliageData itself, authored from its own FoliageTypeEditor tab, rather than as ad hoc
    // fields shared by the WHOLE palette at once -- see OcFoliage.hpp for the format and why this
    // split makes a foliage type a real, reusable asset instead of a global brush setting. Empty no
    // longer refuses mode entry -- see editor::foliageModeGate (FoliageTypeEditor.hpp) -- it means
    // buildFoliageModePanel() shows a create-a-type empty state instead of the palette list.
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

    // WHAT "1" ON THE CAMERA-SPEED DIAL MEANS, in cm/s.
    //
    // flySpeed_ stays in centimetres per second because that is what the movement integration and
    // the saved preference are in, and changing the stored unit would silently reinterpret every
    // editor.ini in existence. This is a DISPLAY scale only: the chip and the slider divide by it,
    // so the default speed reads as "1" rather than "800" -- the number a person tunes by feel, the
    // way Unreal's 1-8 camera speed does, instead of a raw rate they have to convert in their head.
    static constexpr f32 kCamSpeedUnit = 800.0f;
    bool flying_ = false;
    // The pawn currently viewed in FIRST PERSON this frame, or kInvalidEntity -- set by
    // drivePlayCamera(), read by the owner-hide check beside the frustum/occlusion culls. A mesh
    // carrying kMeshRendererHiddenFromOwner skips the rasterised draw when this is itself or an
    // ancestor, and stays untouched for third-person (which uses a boom offset instead).
    // RESET UNCONDITIONALLY AT THE TOP OF drivePlayCamera(), never left stale across an early return:
    // this codebase already lost a session to a handle that outlived its meaning
    // (aver-float-cannot-hold-handles), and a stale handle matching a REUSED one in edit mode is the
    // same bug shape, for a hide flag.
    scene::Entity firstPersonPawn_ = scene::kInvalidEntity;
    // Latched during the scene pass so the outline draws after every surface is down.
    Mat4 selectionOutline_{}; rhi::MeshHandle selectionMesh_ = 0; bool hasSelection_ = false;
    // Every selected entity's (world transform, mesh id) for this frame's outline pass. Rebuilt
    // during the draw walk and cleared right after drawing, so it can never describe a stale set.
    std::vector<std::pair<Mat4, u64>> selectionOutlines_;
    f32 sunColor_[3]={1.0f,0.96f,0.9f}, sunAmbient_=1.0f;
    f32 skyZenith_[3]={0.19f,0.42f,0.78f}, skyHorizon_[3]={0.72f,0.80f,0.90f};
    // A tint on the in-scattered sky (white = clear air), and an extinction per cm.
    f32 fogColor_[3]={1.0f,1.0f,1.0f}, fogDensity_=4e-6f;
#if AVER_MODULE_SCENE
    // OPT-IN (Height Fog panel, only shown while chunk streaming is on): recomputes fogDensity every
    // frame from the streaming load boundary instead of a density chosen once and left to drift out of
    // sync. See the per-frame fog push for the derivation: it solves averFogFactor's k<=1e-8 branch
    // for density, not an approximation. Default OFF: a visibly foggier world, must never become the silent default.
    bool matchFogToStreamRadius_ = false;
    f32  fogMatchTargetOpacity_ = 0.9f;   // opacity WANTED at the load boundary itself
#endif
    rhi::PostSettings post_{};
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
    // The baked grid and the line mesh drawn from it. The mesh is REBUILT on every bake and
    // every load, which is why destroyLineMesh had to exist first: without it each rebuild
    // leaked a committed upload buffer, and the overlay is the one thing here that rebuilds.
    fmt::OcNavData  nav_;
    rhi::LineHandle navMesh_=0;
    bool showNav_=false;
    // Collider overlay: the world-space AABB of every physics body. There was no way to see
    // collision in this editor at all before it -- no toggle, no wireframe, nothing. Persisted
    // through editor.ini like the other view toggles, because it is a property of the VIEW and not
    // of the level.
    bool showColliders_=false;
    // The GPU profiler panel. A view over GpuTimingReport, which the device has always
    // produced and only the console ever read.
    bool showProfiler_=false;
    // The References panel. refPanelScanned_ distinguishes "opened but never asked" from "asked and
    // found nothing" -- two states an empty list cannot tell apart, and the second is the useful one.
    bool showReferences_ = false;
    bool refPanelScanned_ = false;
    std::string refPanelAsset_;
    std::vector<std::string> refPanelResults_;
    rhi::LineHandle colliderMesh_=0;
    bool navRegionColours_=true;
    // --bake-nav: bake once at startup, then carry on. Deferred to a frame rather than done at
    // init because the bake reads PHYSICS BODIES, and a level's bodies are built by
    // applyProject, which has not run when the app is constructed.
    bool navBakeOnStart_=false;
    bool navBakeDone_=false;
    bool navLoadPending_=false;
    f32  navBakeCell_=50.0f;
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
    // The editor's Dear ImGui backend, installed into the device non-owning, the same
    // unique_ptr-owns/raw-pointer-on-the-device shape averSrUpscaler_ uses. UNLIKE that one, onShutdown
    // does NOT reset() this early: the device's uiShutdown() (between onShutdown returning and `delete
    // app`) needs this object still alive when it runs. Resetting early would reproduce the same
    // dangling-raw-pointer bug, just sooner. Natural destruction order (this dies only when
    // SandboxApp does) keeps it safe with no explicit detach.
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
    // --gbuffer: see GBufferDebugFeature's top comment and onUpdate's per-frame setGBufferEnabled
    // push. Kept as a plain override, not applied directly here, because the debug view below must
    // also be able to turn the G-buffer on by ITSELF from the viewport dropdown -- the two are OR'd together every frame.
    bool gbufferOverride_ = false;
    // --gbuffer-debug velocity|viewz|normals, or the matching viewport view-mode dropdown entries:
    // which channel (if any) GBufferDebugFeature draws over the 3D viewport this frame. Off is the
    // default and costs one enum compare, nothing else. THIS is what makes gBufferVelocityTexture()/
    // gBufferViewZTexture()/gBufferNormalRoughnessTexture() probeable (see captureCheck()).
    GBufferDebugFeature::Mode gbufferDebugView_ = GBufferDebugFeature::Mode::Off;
    // Registered once in onInit, unregistered in onShutdown (both unconditional -- this is generic
    // RHI, gated on no module). Never rebuilt: one instance for the whole run, exactly like
    // voxiRenderer_ below.
    GBufferDebugFeature gbufferDebugFeature_;
    bool gbufferDebugAttached_ = false;
// AND AVER_MODULE_SCENE, not just OCCLUSION -- every OTHER `#if AVER_MODULE_OCCLUSION` in this file
// carries the same pair: two members below are keyed on scene::Entity, so OCCLUSION-on/SCENE-off
// named a type that doesn't exist (module-matrix.ps1's scene-off and all-off rows both failed here).
// WRITTEN OUT AT ALL NINE SITES rather than centrally: none of the nine blocks nests inside a SCENE
// region, so guarding only the members would leave readers compiling against vanished members -- the
// same split-guard shape this file has been bitten by before.
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
    // --occlusion-cull: hierarchical-Z two-pass box culling (modules/occlusion). OFF (default) never
    // calls occluder_ or reorders the entity walk -- see renderSceneEntities' comment for the two-pass
    // mechanism, which reorders IN PLACE rather than duplicating the walk depthPrepassOverride_ uses,
    // since the draw logic it reuses (LOD, material binding, GPU-cluster paths) that walk doesn't replicate.
    bool occlusionCullEnabled_ = false;
    bool occlusionCullForceOff_ = false;   // --no-occlusion-cull: see setOcclusionCullForceOff's own comment
    // --occlusion-waitidle / --no-occlusion-waitidle: see setOcclusionDebugForceWaitIdle's own
    // comment. CLI-seeded default only -- once occluder_ exists, EditorConsole.hpp's
    // consoleOcclusionForceWaitIdleSlot() (seeded from this in onInit) is the live source of truth,
    // reasserted onto occluder_ every frame in onUpdate, so a console `set
    // occlusion.debugForceWaitIdle` takes effect the same way either flag does. DEFAULTS TRUE (a
    // later investigation found the no-wait path measurably worse without finding why -- see
    // OcclusionCuller.cpp's FOLLOW-UP comment above its kInFlight member); --no-occlusion-waitidle
    // opts back into the faster, unproven-correct path.
    bool occlusionDebugForceWaitIdleArg_ = true;
    // NON-owning would be wrong here: this module has no registry of its own the way IRenderFeature
    // does, so SandboxApp owns the one instance for the run and destroys it in onShutdown.
    aver::occlusion::IOcclusionCuller* occluder_ = nullptr;
    // Per-entity "the pyramid could not prove this hidden, as of the last time it was tested" bit,
    // carried across frames (this bookkeeping belongs to the CALLER, not the culler -- Occlusion.hpp).
    // Absent means "never tested" and defaults to visible, so a freshly spawned entity is never missing from its first frame on screen.
    std::unordered_map<scene::Entity, bool> occlusionVisible_;
    // This frame's reordering of [0, w.count()) so every PASS-1 (assumed-visible) index precedes
    // every PASS-2 one (see renderSceneEntities). A MEMBER, not a local, purely to reuse its
    // allocation frame to frame rather than reallocating a several-thousand-entry vector every frame.
    std::vector<u32> occlusionOrder_;
    // This frame's world AABBs, collected in a dedicated pre-walk ahead of the main entity loop: the
    // occlusion culler needs ALL of them (not just the pass-2 subset) in one call, computed once up
    // front rather than reusing the main loop's own per-pass-2-entity box. Parallel arrays, kept as members like occlusionOrder_.
    std::vector<aver::occlusion::Aabb> occlusionBoxes_;
    std::vector<scene::Entity> occlusionBoxEntities_;
    std::vector<u8> occlusionResults_;
    // Diagnostics: accumulated since the process started, and the frame count they cover -- see the
    // periodic log line in renderSceneEntities for where these are read and reset.
    u64  occlusionCulledAccum_ = 0, occlusionTestedAccum_ = 0;
    u32  occlusionReportFrames_ = 0;
    bool occlusionWasVisible(scene::Entity e) const;
    // ---- MOTION-SAFE CULLING: the camera basis testBatch()'s (at best one-frame-stale) answer was
    // actually computed from -- see renderSceneEntities' own trust-gate comment above the
    // box-collection loop for the full reasoning, and Occlusion.hpp's corrected "TWO-PASS" section
    // for why an answer needs this at all. Stashed AFTER buildPyramid() runs (inside
    // occlusionBuildAndTest), the SAME idiom chunk streaming already uses for its own "camera value
    // as of last time I looked" bookkeeping -- reused here rather than adding a second accessor to
    // IOcclusionCuller, per this file's own comment on
    // occlusionVisible_ above ("this bookkeeping belongs to the CALLER, not the culler").
    Vec3 occlusionBasisCamPos_{0.0f, 0.0f, 0.0f};
    Vec3 occlusionBasisForward_{1.0f, 0.0f, 0.0f};
    bool occlusionBasisValid_ = false;
    // F7: the scene's own sub-rect (target pixels) that produced THIS basis's pyramid, stashed
    // alongside occlusionBasisCamPos_/occlusionBasisForward_ for the exact same reason -- a dock
    // layout drag between the pyramid being built and its (one-call-stale) readback being consumed
    // is a discontinuity a motion margin cannot cover, same as a teleport. occlusionTrustworthy
    // requires this to still equal THIS frame's own sceneViewport() read.
    f32 occlusionBasisRect_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // F7's once-per-change diagnostic: the last rect/pyramid size the "[Occlusion] testing against
    // scene rect..." line actually printed for, so a static dock layout logs it exactly once instead
    // of every frame culling runs. occlusionLoggedPyramidW_ starts at a value no real texture width
    // will ever equal, so the very first frame culling turns on always logs.
    f32 occlusionLoggedRect_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    u32 occlusionLoggedPyramidW_ = 0xFFFFFFFFu, occlusionLoggedPyramidH_ = 0xFFFFFFFFu;
    // F8: has the "[Occlusion] idle: ..." line already fired for the CURRENT idle streak. Reset to
    // false the moment occlusionRuns is next true, so idle -> running -> idle logs a second time
    // rather than only ever once per process.
    bool occlusionIdleLogged_ = false;
    // 3B: this frame's own copy of the two EditorConsole.hpp slots, reasserted every frame in
    // onUpdate (beside occlusionDebugForceWaitIdle's own reassertion) so onRender's walk reads a
    // value that is live for THIS frame, not last frame's -- see onUpdate's own comment. Both default
    // false, matching the slots' own defaults.
    bool occlusionShowCulled_ = false;
    bool occlusionCullUnderSuppression_ = false;
    // Diagnostics for the staleness detector (OcclusionCuller.cpp's generation-stamp check, surfaced
    // through IOcclusionCuller::readbackLagIsExactlyOneCall()): how many tested frames it fired on, and
    // whether the one-time loud warning has already fired. Folded into the same periodic report as
    // occlusionCulledAccum_ above.
    u64  occlusionStaleReadbacks_ = 0;
    bool occlusionStaleWarnedOnce_ = false;
#endif
    // All three use -1 for "flag not given", NOT 0 -- 0 is Quality::Off and has to be expressible.
    // It was 0 here, so --gi 0/--rt 0/--pt 0 were silently no-ops through both the startup path and
    // applyProjectRenderSettings; --no-gi/--no-rt existed only as workarounds, and path tracing never got one.
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
    int  ptBouncesOverride_=-1;       // --pt-bounces N (-1 = flag not given)
    int  layeredBsdfOverride_=-1;     // --layered-bsdf N (-1 = flag not given)
    f32  coatWeight_=0.0f;            // --coat W [R] [F0]: 0 = flag not given, and no coat anywhere
    f32  coatRough_=0.1f;
    f32  coatF0_=0.04f;
    int  giUpdateIntervalOverride_=0; // --gi-update-interval N: GI revoxelise interval (0 = flag not given)
    // --gi-mode N: indirect-diffuse estimator, 0 = voxel cones, 1 = RTXDI ReSTIR GI (Settings::giMode).
    // Sentinel is -1, NOT 0 like its neighbour above -- 0 is a real, meaningful VALUE here ("voxel
    // cones"), not "flag not given", so the giUpdateIntervalOverride_ convention would silently make
    // `--gi-mode 0` indistinguishable from never passing the flag at all.
    int  giModeOverride_=-1;
    // --restir-visibility none|reconstructed|half|full: optimisation-wave-2's U1 (Settings::
    // giRestirVisibility). Same -1-is-absent sentinel and reasoning as giModeOverride_ just above --
    // 0 (NoRay) is a real, meaningful value, not "flag not given".
    int  restirVisibilityOverride_=-1;
    int  denoiserOverride_=-1;       // --denoiser 0|1: -1 is "flag not given"; see setDenoiser
    int  reblurAccumOverride_=-1;    // --reblur-accum N: REBLUR_DIFFUSE history depth for a --frames run; -1 is "flag not given"
    f32  renderScaleOverride_=1.0f;  // --render-scale F: scene render resolution as a fraction of present, clamped [0.25,1]
#if AVER_MODULE_SR
    // --aversr LEVEL / the render-settings quality combo. Off (default) is what a build with no
    // AverSR looks like: no render-scale change beyond --render-scale itself, no SpatialUpscaler
    // construction. See docs/AVERSR.md and onInit()/applyAverSrQuality() for where it's read.
    aver::sr::Quality averSrQuality_ = aver::sr::Quality::Off;
    // True when --aversr set the value above, so a stored preference does not overwrite the command
    // line. Persisted state loses to a flag everywhere else in this file; this makes it true here.
    // ALSO true for --aversr auto (setAverSrCliAuto): "CLI wins" still means loadEditorPreferences
    // must not apply a stored preference over it, even though auto has no single level of its own to
    // pin -- averSrCliLevel_ stays -1 in that case and updateAverSrAuto lets the manifest/ladder chain
    // decide the level every frame, per --aversr auto's own "auto means explicit CLI Auto: user
    // preferences are ignored; manifest and ladder apply" contract (plan section 3.3 A).
    bool averSrFromCli_ = false;
    bool averSrCliAuto_ = false;      // --aversr auto: see averSrFromCli_'s own comment just above
    int  averSrCliLevel_ = -1;        // --aversr LEVEL (not auto): the pinned level, for
                                       // updateAverSrAuto's resolveAverSrLevel call every frame
    // optimisation-wave-2, U2/3.3 A: the user's own Display preference, superset of averSrQuality_
    // (Auto and Manual besides the four named levels -- see AverSrChoice.hpp's own top comment for
    // why aver::sr::Quality alone cannot carry either). Loaded once from display.aversrChoice (via
    // migrateAverSrChoice the first time that key is absent), written by the Display combo, and read
    // every frame by updateAverSrAuto as the "user" rung of the CLI > user > manifest > auto chain.
    editor::AverSrChoice averSrChoice_ = editor::AverSrChoice::Auto;
    // Set once, the session a stored display.renderScalePending cookie is found still armed at load
    // (3.3 A's own "a named level did not survive its own launch" case, the non-Manual sibling of the
    // pre-existing renderScaleCookieArmed_/display.renderScalePending dance) -- forces Off for the
    // REST OF THIS SESSION regardless of what averSrChoice_/the manifest/the ladder would otherwise
    // resolve to, so a level that just took the device down is never silently re-attempted a frame
    // later. Cleared by nothing; a fresh launch is what re-arms the chance to try again.
    bool averSrCookieTripped_ = false;
    // Set true only when migrateAverSrChoice ran on a genuinely ABSENT display.aversrChoice key AND
    // landed on Auto -- i.e. THIS is the first session after migrating into U2's new default. Cleared
    // the moment the user picks any item on the Display AverSR combo or the Project Settings
    // "Upscaling default" combo, per 3.3 A's "until the user picks any item" rule. Read by both of
    // those surfaces to show the one-time amber note; never written back to disk itself (the
    // migration is inferred fresh from the pref keys every load, not remembered as its own flag).
    bool averSrMigrationNoteArmed_ = false;
    // Set the first time updateAverSrAuto is about to apply a non-Off level THIS SESSION -- arms
    // display.renderScalePending (and renderScaleCookieArmed_, the pre-existing 30-frame clear) before
    // that application, the same crash-cookie protection the Manual/named-level LOAD path already
    // gives a stored render scale, extended to cover a level Auto resolves to on its own mid-session.
    bool averSrArmedNonOffOnce_ = false;
    // Fires the "[AverSR] ... scene WxH -> present WxH" line exactly once per process (3.3 A's
    // mandatory startup log, required on every run including --frames) -- see updateAverSrAuto.
    bool averSrStartupLogged_ = false;
    // Why the CURRENTLY APPLIED level is what it is -- Auto/Manifest/User/Cli/ForcedOff
    // (Scalability.hpp's AverSrSource), read back by the Project Settings upscaling line and the
    // Display combo's "Auto (<level> from <source>)" label. Written only by updateAverSrAuto, which
    // runs every frame, so this is never stale by more than one frame.
    voxi::AverSrSource averSrSource_ = voxi::AverSrSource::Auto;
    // The project's own AverSR default combo's live edit state (Project Settings > Rendering, page 1)
    // -- mirrors project_.averSr the same way occlusionCullEnabled_ mirrors project_.occlusionCull,
    // EXCEPT unconditionally on every project open rather than only when the manifest states a value:
    // -1 (follow the Overall preset) is itself a meaningful, explicit combo choice here, not merely
    // "unstated", so a project that does not pin one must reset this back to -1 rather than silently
    // inheriting whatever the PREVIOUS project's pin was (applyProjectRenderSettings' own "mirrors,
    // not a delta" comment on projectBackend_/frameBudgetMs_ describes the identical hazard). Captured
    // back into project_.averSr beside project_.occlusionCull in captureRenderSettingsFromUi.
    int  averSrProjectDefault_ = -1;
    // Constructed lazily the first time a non-Off quality is applied; never rebuilt after, only
    // dropped to null when quality returns to Off. `factory` must outlive every execute() call
    // (AverSrSpatial.hpp): satisfied here since it's the SAME rhi::IDevice::resources() the editor uses for as long as the device exists.
    std::unique_ptr<aver::sr::SpatialUpscaler> averSrUpscaler_;
    // --edge-aa: constructs a real aver::sr::FxaaResolve the SAME way averSrUpscaler_ does for
    // SpatialUpscaler. OFF (default) never constructs one, keeping an unused build bit-identical.
    // SHARES ONE rhi::IDevice UPSCALER SLOT WITH AverSR (setUpscaler takes one pointer, not a list), so
    // only one of --aversr/--edge-aa runs per frame; applyUpscalerSlot() decides which wins -- not a
    // limitation this task's measurements hit, since none of its four configurations use --aversr.
    bool edgeAaEnabled_ = false;
    std::unique_ptr<aver::sr::FxaaResolve> edgeAaUpscaler_;
    // Said once per session, not once per frame: updateAverSrAuto forces Auto to Off every single
    // frame --edge-aa occupies the upscaler slot, and re-logging that every frame would flood the log
    // the instant both are live at once.
    bool edgeAaAverSrWarnLogged_ = false;
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
    f32  camTranslateSpeed_=0.0f;    // --cam-translate SPEED: forward-flight, cm/frame, 0 = no motion
    // --mesh-heap default|upload (W4): false (DEFAULT) = every new static mesh's vertex/index buffers
    // go on the Upload heap, today's behaviour on every backend. true moves them to the Default heap
    // instead -- see rhi::IDevice::setStaticMeshHeapDefault's own comment for the trade. Applies to
    // createMesh calls made AFTER it is set, so it is read once, at the top of loadProjectMeshes,
    // before that call's first mesh upload -- not reasserted per frame, since meshes load once.
    bool meshHeapDefault_ = false;
    // --lod-share-vertices 0|1 (W11): false (DEFAULT) = today's behaviour, every coarser LOD level
    // gets its own independent vertex buffer even though it duplicates LOD0's vertex data untouched.
    // true shares LOD0's vertex buffer across every level via createMeshSharingVertices, falling back
    // to the independent-buffer path on any refusal (unsupported backend, or the mesh's own vertices
    // are compute-written). Read inside loadProjectMeshes' LOD-ladder loop, once per mesh at load time.
    bool lodShareVertices_ = false;

    Vec3 camPosOverride_{};
    f32  pitchOverride_=0.0f, yawOverride_=0.0f;   // radians, converted in setCamera
    bool useWarp_=false;             // --warp: run on the D3D12 software rasteriser
    // The engine, for the log sink to write the startup splash through. Set at the top of onInit and
    // left set: loadingScreenActive() is what actually gates the write, and Engine clears that itself
    // when the splash closes, so there is one owner of "is it still up" rather than two guesses.
    Engine* engineForSplash_ = nullptr;
    std::thread::id mainThreadId_{};
    bool windowedOverride_=false;    // --windowed: kept so the flag still parses; windowed is the default now
    bool fullscreenOverride_=false;  // --fullscreen: opt an interactive run INTO borderless fullscreen
    std::string backendName_;   // --backend: which RHI backend to ask for first
    // RENDER.BACKEND as the OPEN PROJECT states it. Separate from backendName_, which is what
    // this RUN was actually launched with: editing the project's choice must not retarget the
    // device under a running editor, and showing the running backend beside the setting is only
    // honest if the two are distinct values.
    std::string projectBackend_;
    std::string runningBackend_ = "?";   // what the device actually came back as, latched at init
    bool debugLayer_=false;          // --debug-layer: validate every graphics call (a real per-call tax)
    std::string scriptsDir_;         // --scripts <dir>: where to look for user script assemblies
    std::string spawnTestClass_;     // --spawn-test <ClassName>: headless actor-loop test trigger
    bool spawnTestDone_=false;       // the spawn is one-shot, done on the first frame scripts are ready
    int32_t spawnTestEntity_=0;      // the spawned test entity, destroyed a few frames later
    int spawnTestFrames_=0;          // frames since the test spawn, so the destroy is one-shot too
    // The open drawer's CURRENT animated height in pixels, 0 when closed. Written by drawDrawer, read
    // by the viewport hint so it can sit above the drawer. Kept as a published value rather than
    // recomputed at the hint's own site, since a second copy of that arithmetic would drift.
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
    // ONE PENDING OPEN, whatever asked for it. Three things now want to open a level -- File > Open
    // Level's picker, a double-click in the Content Browser, and a path forwarded from a second
    // launch -- and only the last of those used to exist. They are funnelled through one request so
    // the unsaved-changes guard and the class-placement spawn happen once each, in one place,
    // instead of once per caller with the third one forgetting.
    //
    // DEFERRED RATHER THAN IMMEDIATE, because cbOpenEntry (the Content Browser) has no Engine& to
    // hand loadLevel -- nothing in this class stores one. The request is latched here and drained in
    // onUpdate, which does, exactly as the forwarded-open poll beside it already works.
    std::string pendingOpenPath_;
    std::string pendingOpenWhy_;      // how it was asked for, for the modal's own sentence
    // The modal shown when a pending open would discard unsaved work. Separate from exitPrompt_
    // rather than reusing it: exiting the editor and switching levels mid-session are two different
    // destructive actions, and one modal meaning both risked "Discard and exit" when what is about
    // to happen is "open a different level".
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
    // The tile counts, named once each. They used to be two unconnected literal 4s -- one at the
    // load call and one at the blit -- so changing either alone sampled the wrong UV window and
    // every icon silently shifted.
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
#if AVER_MODULE_VOXI
    voxi::VoxiRenderer voxiRenderer_;
    bool voxiAttached_=false;
#endif
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // Registration is non-owning, same contract as voxiRenderer_ just above: particleRenderer_ must
    // outlive the device, torn down in onShutdown.
    particles::ParticleRenderer particleRenderer_;
    bool particlesAttached_=false;
#endif

    // Water is an INDEPENDENT module from Particles (see the include-block comment near the top of
    // this file for the full story) -- b6881c49 nested this whole group inside
    // `AVER_MODULE_PARTICLES && AVER_MODULE_SCENE` above, so a particles-off, fluids-on tree declared
    // none of these members while setWater() (guarded correctly, on AVER_MODULE_
    // FLUIDS alone) still used them -- undeclared-identifier errors, not a missing-header ones, which
    // is why this half of the bug survived fixing only the includes.
    //
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

    static void animNotify(scene::Entity e, const char* name, void* user);

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
    std::unique_ptr<aver::editor::SkinDrawTest> skinDraw_;
#if AVER_MODULE_SCENE
    // The scene join: gives every entity with a CSkeletalMesh its own posed mesh. Null when the
    // skinning shader would not compile, in which case skinned entities simply draw at rest.
    std::unique_ptr<aver::render::SkinnedScene> skinnedScene_;
    // The content browser's rendered mesh thumbnails -- a small ActorPreview of its own, kept rather
    // than re-shared with skinnedScene_ or the asset editors' preview, since sharing either would
    // fight this one for its draw list or force every thumbnail to a size it isn't. A value member,
    // matching particleRenderer_'s declaration: init() can still fail, but nothing else needs the object to not exist, only to be inert.
    aver::editor::ThumbnailCache thumbnails_;
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
    // existing --frame-time/GPU-marker path measures CPU simulation separately from the GPU draw.
    // Accumulated every frame and printed once at shutdown; the steady_clock call costs single-digit
    // nanoseconds and stays always-on to prove it didn't skew any earlier probes.
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
    // one flag that matters now (see its own comment) -- there used to be a separate ptSceneViewFlag_
    // here too, but nothing ever read it once syncPtSceneView() took over registration, so it was
    // dead weight kept only by the refactor that introduced the want-flag. Removed.
    std::unique_ptr<aver::pt::PtSceneView> ptSceneView_;
    // ptSceneViewWantEnabled_ is the REQUESTED state (--pt-scene at startup, or the settings-page
    // Quality combo at any later frame); ptSceneView_ != nullptr is the ACTUAL one. syncPtSceneView()
    // reconciles the two, only ever from onUpdate(), before beginFrame(). Deliberately NOT itself a
    // read of voxi::Settings::pathTracing: this flag and PT's registration must keep working with AVER_MODULE_VOXI off.
    bool ptSceneViewWantEnabled_ = false;
#if AVER_MODULE_VOXI
    // N8: the Path Tracing tier onUpdate's own reconcile block (above ptSceneViewWantEnabled_'s own
    // syncPtSceneView call) last saw vx.settings().pathTracing read as. Guarded on AVER_MODULE_VOXI,
    // unlike ptSceneViewWantEnabled_ itself, because its TYPE is voxi::Quality -- this member cannot
    // exist at all in a build without Voxi, which is exactly why the reconcile block that reads it is
    // guarded the same way and every OTHER member on this page stays outside the guard.
    voxi::Quality ptTierSeen_ = voxi::Quality::Off;
#endif
    // --pt-scene WAS GIVEN ON THE COMMAND LINE. Sticky for the session, separate from the want flag
    // above because that flag is written by four different things (CLI, settings combo, toggle-test
    // flags, a project manifest), and this answers which ONE asked -- see applyProjectRenderSettings, where a manifest used to silently outrank a typed flag.
    bool ptSceneViewFromCli_ = false;
    bool ptSceneViewUnavailable_ = false;   // init() refused once this session -- stop re-asking
    // A1: true while syncPtSceneView() is holding ptSceneViewWantEnabled_ down because ray-driven
    // primary visibility is painting the scene -- set/cleared in the SAME block that already decides
    // it (see ptSceneViewYieldLogged_'s neighbouring flag, which only tracks whether the log line
    // fired once). Read by the Path Tracing page's Quality-combo tag (see PtRenderConflict.hpp's
    // choosePtViewTag) so the UI says why the combo does nothing, instead of nothing at all.
    bool ptSceneViewSuppressedByRayDriven_ = false;
    // --pt-scene-toggle-on/--pt-scene-toggle-off [N]: VERIFICATION ONLY. Simulates a human flipping
    // the Path Tracing settings-page Quality combo N frames into a bounded run, proving the RUNTIME
    // toggle (register/unregister mid-session, not just --pt-scene's register-before-frame-1 path)
    // without a human clicking. Two independent countdowns from process start, not "N frames after the ON one", so the caller picks values (e.g. on=5, off=15) freely.
    // --pt-quality-ramp [N]: 0 is off. See the ramp in onUpdate.
    int ptQualityRampEvery_ = 0;
    int ptQualityRampCountdown_ = 0;
    int ptSceneToggleOnAutoFrames_ = 0;
    int ptSceneToggleOffAutoFrames_ = 0;
    // --sun-set-at N ELEV AZIM and --gi-history-reset-at N: VERIFICATION ONLY. Simulate a human dragging
    // the Directional Light panel's Elevation/Azimuth sliders, and typing resetgihistory +
    // resetnrdhistory, N frames into a bounded --frames run -- the only way to capture what indirect
    // light does in the frames AFTER a live sun move, which a level's own SUN line (applied before the
    // first frame) can never show. Countdowns from process start, the same shape as the two above.
    int sunSetAtFrames_ = 0;
    f32 sunSetElevDeg_ = 0.0f, sunSetAzimDeg_ = 0.0f;
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
    // A row asked to be deleted; answered after the tree walk. See drawOutlinerRow.
    scene::Entity outlinerDeleteRequest_ = scene::kInvalidEntity;
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
    // The loaded level's own header, placements and PCG volumes stripped.
    // THE SAME REASONING AS levelPcgVolumes_, GENERALISED: saveLevel used to build a fresh OcWorldData
    // from the editor's state, so every field the editor doesn't model reset to a default on write
    // (SPAWN deleted, BUILD back to 0, ID recomputed, `lux` reverted to 100000). Starting the save
    // from what the file actually said, overwriting only what the editor genuinely owns, fixes all of
    // those at once -- and any field added to the format later, which enumerating them would not.
#if AVER_FLUIDS_SIMULATED
    // ---- the 3D-viewport icon renderer, and the Player Start marker it draws ----
    // viewportIconsReady_ is the ONE flag the render walk consults to decide whether to skip the
    // Player Start's cube, false unless the feature AND its texture both came up -- every failure path lands on the same behaviour, drawing the cube as before.
    editor::ViewportIconRenderer viewportIcons_;
    bool viewportIconsReady_ = false;
    editor::ViewportIconRenderer::IconHandle playerStartIcon_ = editor::ViewportIconRenderer::kNoIcon;
#endif
    fmt::OcWorldData levelHeader_;
    // Whether each level entity's placement said `nocollide`. There is NO component for this: it is
    // a load-time instruction and nothing on the entity records it afterwards, so without this the
    // save forced `collide = true` on everything and `nocollide` never survived a round trip.
    std::unordered_map<u32, bool> entityCollide_;
    // Entities placed with `snap`, and the AUTHORED z offset each was placed at. Absent = not snapped.
    std::unordered_map<u32, f64> entitySnapZ_;

    // TRUE WHILE THE OPEN LEVEL WAS LOADED THROUGH THE LEGACY OCMAP PATH (onLegacyOcmapInstantiated),
    // rather than the ordinary OCWORLD one -- decided by which RECORDS the file uses, not its header
    // or extension. saveLevel reads it to choose which writer owns the file: a level loaded as OCMAP
    // must be saved as OCMAP, or ROOT/CLIENT/SURFACE/GROUND/KILLZ/DEFORM survive the read only to be
    // dropped on the next write. Reset to false by unloadLevel, so a later .ocworld is never mistaken for a legacy map.
    bool levelIsLegacyOcmap_ = false;
    // THE LOADED LEVEL'S OWN LEGACY HEADER, placements stripped -- the exact levelHeader_ pattern
    // above, generalised to the five record kinds only OCMAP has and this editor has no UI for: ROOT,
    // CLIENT, the SURFACE table, GROUND and KILLZ. saveLevelAsOcmap starts from this and overwrites
    // only NAME/ID/BUILD/ALGO/SPAWN and the placement list, so an unshowable field still survives every save.
    fmt::OcMapData legacyMapHeader_;
    // Which record kind each legacy-loaded entity came from (true = DEFORM, false/absent = PLACE),
    // and that record's own field the ordinary OcWorldPlacement round trip has no room for: a PLACE's
    // numeric SURFACE-table index (entityLegacySurface_; -1 = "the asset's own") or a DEFORM's
    // soft-body material name (entityLegacyMaterial_, e.g. "rubber") -- see onLegacyOcmapInstantiated's
    // "MATERIAL IS DELIBERATELY LEFT EMPTY" comment for why neither reaches the CMeshRenderer. An
    // entity absent from entityLegacyDeform_ saves as an ordinary PLACE with surface -1.
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
    // The ASSET id of the selected mesh (what meshPathById_ and the outline cache key on), as
    // distinct from selectionMesh_, which is a GPU upload handle nothing can turn back into a file.
    u64 selectionMeshId_ = 0;
#endif

    // ---------------- landscape (opt-in; --landscape <path>, or <levelname>.ocland beside the level) ----------------
    // Hosts ONE open .ocland section, through game::GameLandscape below: render, collision,
    // level-reference AND sculpt (see LANDSCAPE_EDITOR.md slice 0 for what's still missing: an
    // asset-editor tab). Independent of AVER_MODULE_SCENE -- a section is not an ECS entity.
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
    // Turns chunk streaming on or off around the editor camera. OPT-IN: nothing in modules/world's
    // generator or region files is touched until a user flips Window > Chunk Streaming, so an
    // ordinary project opens exactly as it always did.

    static f32 fogDensityForOpacityAt(f32 distanceCm, f32 targetOpacity);

    // GUARDED: scene::Entity doesn't exist with AVER_MODULE_SCENE=OFF, and its only caller is
    // already inside a SCENE guard -- missing it broke the scene-off row of module-matrix.ps1, the
    // only thing that checks this.
#if AVER_MODULE_SCENE
    bool anyChunkWorldOwns(scene::Entity e) const;
#endif

    void setChunkStreamingEnabled(bool on);

    void setDroneEnabled(bool on);

    u64 residentTriangleCount() const;

    // A small always-on-while-streaming readout of StreamStats. pendingLoads and failedLoads are
    // singled out because they are the two numbers that tell "working" (pendingLoads draining, zero
    // failures) from "not keeping up" (pendingLoads staying high) or "broken" (failedLoads growing).
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

    bool requestOpenLevel(const std::string& path, const char* why);

    void applyPendingOpen(Engine& eng);

    void startNewLevel(Engine& eng);

    void openLevelDirect(Engine& eng, const std::string& path);

    void loadStartMap(Engine& eng);

    void unloadLevel(Engine& eng);

    bool saveLevel(const std::string& path);

    bool saveLevelAsOcmap(const std::string& path);

    // ---- WHAT PLAY IS ALLOWED TO CHANGE, AND WHAT STOP PUTS BACK ----
    // Play used to be a one-way door: aver_fw_end_play() left the WORLD exactly as gameplay left it,
    // so pressing Stop returned you to a level no longer the one you'd opened, recoverable only by
    // reloading and losing your edits.
    // A TRANSFORM SNAPSHOT, NOT A RELOAD: reloading would also throw away every unsaved edit made
    // before Play, a worse and silent bug. Restoring from memory keeps the editor's own state untouched.
    // TRANSFORMS, VISIBILITY AND SPAWNED ENTITIES, deliberately, not a full component snapshot:
    // those three cover what physics and gameplay actually change; the honest subset, so nobody
    // reads Stop as a guarantee it doesn't make. Visibility joined the set once it became AUTHORED,
    // saved data rather than throwaway session state -- a graph can legitimately call
    // Entity.SetVisible during Play (the identical kMeshRendererVisible bit the Details panel's
    // Visible checkbox now saves with the level), and without this Stop left that toggle standing:
    // hide a prop from a trigger, press Stop, and the editor's own saved state came back changed by
    // whatever the last session happened to do to it.
    // True while Play runs on the ENGINE's default GameMode because the project declared none.
    bool defaultPawnPlay_ = false;
    struct PlaySavedTransform { scene::Entity e; Transform xf; bool visible = true; };
    std::vector<PlaySavedTransform> playWorldSnapshot_;
    // Every entity alive when Play began. Anything alive at Stop that is NOT in here was created
    // during play and is taken back down.
    std::vector<scene::Entity> playPreexisting_;
    bool playWorldCaptured_ = false;

    std::vector<scene::Entity> levelEntities_;
    std::string levelPath_, levelName_;
    bool hasLevelSun_ = false;
    bool hasLevelSky_ = false;
    bool hasLevelFog_ = false;
    // "This level said something about clouds", which is NOT the same as "this level has clouds" --
    // see fmt::OcWorldEnv::hasClouds. Without the distinction, saving an overcast level that had
    // been switched to clear would write no record and it would come back overcast.
    bool hasLevelClouds_ = false;

#if AVER_MODULE_FRAMEWORK
    // GRAPH-AS-CLASS / any other class placement: level_.classPlacements() holds what the load
    // found; spawnClassPlacements spawns them and fills levelClassInstances_.
    //
    // WHICH PLACEMENT EACH LIVE INSTANCE CAME FROM, carried explicitly rather than by position.
    // This used to be a bare vector<int32_t> read as index-parallel -- wrong: spawnClassPlacements
    // `continue`s past a class it cannot resolve WITHOUT pushing, so once any placement names an
    // undeclared class (a warning, not an error), every later entity pairs with the wrong placement.
    using ClassInstance = editor::LevelClassInstance;
    std::vector<ClassInstance> levelClassInstances_;
#endif

    // ---------------- chunk streaming (opt-in, Window > Chunk Streaming) ----------------
    // PCG chunk streaming around the editor camera and the drone, shared with the runtime; its
    // entities are transient, never saved or undo-tracked.
    game::GameStreaming streaming_;
#if AVER_MODULE_PHYSICS
    std::vector<int32_t> levelBodies_;
#endif

    // ---------------- graph-driven drone (opt-in, Window > Drone or --drone) ----------------
    // Proves a native scene can be driven by an .ocgraph end to end: spawned by setDroneEnabled(true),
    // ticked via scripts_.graphTick(), released by (false). Deliberately disjoint from levelEntities_
    // for the same reason streaming_'s entities are: transient, never saved, never undo-tracked.
    scene::Entity droneEntity_ = scene::kInvalidEntity;
    // Whether PLAY started this drone, as opposed to the user switching it on from Window > Drone.
    // Stop takes down only the former: ending play should not remove something the user started for
    // their own reasons, and without this flag there is no way to tell the two apart.
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
    // Until this existed, ImGuiIO was the SOLE source of truth for keyboard/mouse in the editor, since
    // SandboxApp never called Window::setEventCallback -- so a correct, tested accumulator
    // (aver::InputState) was wired up by exactly one host, GameApp, a library with no executable.
    // IT DOES NOT COMPETE WITH ImGui: ImGui's Win32 backend hooks messageHook, this uses the separate
    // setEventCallback slot, and imgui_impl_win32 returns 0 (not consumed) for the relevant messages
    // so both see them. A PROPERTY OF VENDORED THIRD-PARTY CODE an upgrade could silently change --
    // why --input-source-test asserts the two agree rather than a comment claiming they do.
    // THE SPLIT IS POLICY vs VALUE: ImGui still decides WHETHER gameplay may have input; this owns
    // WHAT the input is.
    InputState input_;

    // This frame's answer to "who owns the keyboard and mouse", recomputed once per frame just before
    // pushInput. Consumers READ this rather than re-deriving it from ImGui flags (InputOwnership.hpp's
    // eleven-spellings problem). Migrated consumer by consumer: one wrong central function beats eleven independently wrong ones.
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
#endif // AVER_MODULE_MCP
    bool releasedByUser_ = false;   // Shift+F1 during a session; cleared when the session ends

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
    bool                   graphPrintOverlay_ = true;
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
    // which), never because two kinds interpret it the same way: the content browser treats it as a
    // folder path, the console (see drawConsoleTranscriptTab) treats it as text to seed the input box.
    std::string         drawerStartSub_;
#if AVER_MODULE_SCENE
    // Mesh handles, bounds and per-material parts are content_'s (meshFor, boundsFor, partsFor).
    std::unordered_map<u64, std::string>     meshPathById_;   // id -> project-relative path
    // THE TWO WAYS THE SCENE WALK DROPS AN ENTITY, each reported once. Keyed differently on purpose:
    // the invisible-bit fault belongs to an ENTITY (its own component is mis-seeded) while an
    // unresolved id belongs to the ID (every entity naming it shares one fault). Never cleared on
    // level unload -- a second report after a reload would be the same fault, not a new one.
    std::unordered_set<u64> undrawnInvisible_;
    std::unordered_set<u64> undrawnMissingMesh_;
    // Triangle count per mesh id, same key as content_'s meshes. Exists so a resident
    // triangle BUDGET can be reported instead of guessed -- a scattered pine forest's cost is
    // otherwise invisible, and a scatter palette can only be tuned "by looking" without it.
    std::unordered_map<u64, u32> meshTris_;
    // The .ocmesh ids that carry skin weights -- a SKELETAL mesh, Unreal's vocabulary, coloured
    // differently in the Content Browser. Recorded rather than re-read: loadProjectMeshes has already
    // parsed the file by the time it knows this, otherwise reachable only by opening every mesh.
    std::unordered_set<u64> skinnedMeshIds_;
    // Per-mesh triangle data for pick() (ViewportPick.hpp), keyed the same as sceneMeshes_/meshBounds_.
    // Built-ins are filled at creation, above (:2113-2142); a project mesh id is filled LAZILY, by pickGeometryFor,
    // the first time a click ray actually reaches it -- loadProjectMeshes discards its CPU-side
    // OcMeshData once the GPU upload is done (same reason selOutlineLines_ re-reads on demand), so
    // paying for every mesh's positions/normals/indices up front would spend memory on meshes no click
    // ever tests. An id mapped to an EMPTY PickGeometry means "tried and unavailable" -- pick() falls
    // back to that mesh's bounding box, and the empty entry stops the failed load from being retried
    // on every later click.
    std::unordered_map<u64, aver::editor::PickGeometry> pickGeometry_;
    int lastSceneDrawn_=-1;           // last scene-entity draw count, so the log line fires only on change
    // startupComplete's settle detector; see it for why these are mutable and why a frame count.
    mutable int startupSettleCount_ = -2;   // -2 so it cannot match lastSceneDrawn_'s -1 start
    mutable int startupSettleFrames_ = 0;
    // The project-open loading screen, alive from applyProject until the scene settles. Null the
    // rest of the time; see applyProject for why it is not a local any more.
    std::unique_ptr<struct LoadingScreen> projectLoading_;
    // Frames the OWNED project loading screen has been up; see onRender for why it is capped.
    int projectLoadingFrames_ = 0;
    int lastSceneCulled_=-1;          // and the cull count, so a frustum bug shows as a number rather than a gap
    int lastSceneOwnerHidden_=-1;     // and the owner-hide count, so a stuck `hidden=owner` mesh shows as a number too
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    // Per-mesh LOD ladder for virtualized-geometry selection. Populated in loadProjectMeshes ONLY for
    // a mesh the Cook wrote coarserLods for (lodCount() > 1); a mesh with no entry always draws its
    // LOD-0 handle. Every vector is indexed by LEVEL: [0] mirrors sceneMeshes_[id]/meshTris_[id]
    // exactly, [i>0] is the Cook's coarserLods[i-1] -- ITS OWN index buffer against the SAME vertex
    // buffer level 0 uses, so this only ever duplicates INDEX data, never vertices.
    struct MeshLodLadder {
        std::vector<rhi::MeshHandle> handles;   // [level] -> whole-level MeshHandle
        std::vector<u32> triCounts;              // [level] -> that level's own triangle count
        std::vector<f32> errorCm;                // [level] -> aver::trifactor::levelWorldErrorCm(mesh, level)
        // [level] -> that level's meshlets, decoded to ClusterView (bounds + cone only, no vertex
        // data) -- kept resident so the per-frame pass can run the REAL, tested
        // selectVisibleClustersWithStats for informational telemetry on the level actually chosen,
        // without re-parsing the .ocmesh every frame. Counted but NOT (yet) subtracted from what gets drawn.
        std::vector<std::vector<trifactor::ClusterView>> clusters;
    };
    std::unordered_map<u64, MeshLodLadder> meshLods_;

    // ON by default since the cost of leaving it off was measured: every instance was drawing LOD 0
    // no matter how far away it was, which is the entire thing the Cook builds a ladder to avoid.
    // --no-lod-select restores the old behaviour. See setLodSelect for the numbers.
    bool lodSelectEnabled_ = true;      // --lod-select / editor toggle. OFF reproduces pre-existing
                                        // behaviour EXACTLY: every instance draws sceneMeshes_[id]
                                        // (LOD 0), the same handle and code path as before this file.
    f32  lodErrorThresholdPx_ = 1.0f;   // --lod-error-px <n>; pixels of projected screen error
                                        // tolerated before a coarser level is preferred. Same unit
                                        // ClusterSelect.hpp's inCut/screenSpaceErrorPx compare against.
    // --lod-cluster-stats: OFF by default, on purpose: it gates ONLY the informational per-meshlet
    // frustum/cone-cull telemetry below (a real, extra per-instance CPU cost). Keeping it separate
    // from lodSelectEnabled_ means the primary --lod-select frame-time comparison measures ONLY the
    // level-selection draw-call change, not this telemetry's own CPU cost on top of it.
    bool lodClusterStatsEnabled_ = false;
    // This frame's selection counters, logged under "[LOD-SELECT]" whenever any changes.
    // trianglesBeforeLod0 vs trianglesAfterLevel actually predicts frame time (every "after" triangle
    // reaches a drawMesh call); the cluster-cull counters are real, measured telemetry but NOT yet
    // subtracted from "after" -- this slice selects per-level, not per-cluster.
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

    // PER-CLUSTER selection -- what actually makes this virtualized geometry instead of discrete LOD
    // with generated levels (see ClusterAdapt.hpp's "PER-CLUSTER, at last" section). Switchable
    // against the per-level path above (--lod-per-cluster) so before/after is one flag on the SAME
    // build, not a separate compile -- both paths ship in every binary.

    // Every meshlet of a mesh, across EVERY LOD level at once, mesh-local space, plus its expanded
    // GLOBAL triangle-index list -- built once at load time, same gate as MeshLodLadder. `verts` is a
    // COPY of the same vertex array createMesh was first called with; every cluster from every level
    // indexes it, so ONE copy serves the whole DAG and every cut an instance can select.
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

    // ONE cut-assembled MeshHandle PER INSTANCE (not per mesh -- two instances of the same mesh at
    // different distances select different clusters), rebuilt only when the selected cluster-id set
    // actually changes. Keyed by scene::Entity; `lastUsedFrame` is how staleEntityCacheSweep below
    // reclaims a handle whose entity stopped appearing, without hooking every destruction explicitly.
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
    // -1 = decide from DeviceCaps (the default), 0 = --no-lod-mesh-shader, 1 = --lod-mesh-shader.
    // Unguarded like chunkStreamAutoFrames_: the setter is called from unguarded flag parsing, so the
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

    // Informational counters for --lod-mesh-shader ("[LOD-MESH-SHADER]"). NOT a GPU readback: computed
    // by running the SAME, already-tested CPU reference over the SAME clusters/budget the GPU dispatch
    // just used, purely for telemetry. Because ASMain/MSClusterMain are byte-for-byte ports of the CPU
    // functions, what this counts is what the GPU actually drew -- but it's still CPU arithmetic
    // producing the number, not GPU readback, said plainly in the report.
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

    // OUTSIDE EVERY MODULE GUARD, and it has to be. The map is POPULATED beside the Trifactor LOD
    // ladder, but READ from the unguarded call that hands the resolver to Voxi -- declaring it next to
    // what fills it put the member behind AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR and broke both
    // scene-off and trifactor-off. The guard belongs where a thing is BUILT, never where it's declared.
    // Degrading is the point: with no Trifactor the map is empty, every lookup answers 0, and every pass draws the mesh it was given.
    std::unordered_map<rhi::MeshHandle, rhi::MeshHandle> depthProxy_;

    static rhi::MeshHandle depthProxyLookup(rhi::MeshHandle mesh, void* user);

    u32 sceneWalkReports_ = 0;     // scene walks so far; the cost split reports at 2^n of them

    Mat4 invVP_, viewProj_; Vec3 eye_{0,0,0};
};

} // namespace aver
