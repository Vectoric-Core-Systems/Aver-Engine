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
// Shared runtime content component.
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
// Set when Aver.Audio.Abi links in. Not renamed: tests/editor relies on it.
#  include "aver/audio/audio_abi.h"
#endif
#include "aver/formats/OcProject.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/formats/OcMesh.hpp"
// Behaviour-tree format (.ocbt).
#include "aver/formats/OcBt.hpp"
#include "aver/formats/GltfImport.hpp"
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
#include "FoliageAlign.hpp"
#include "PlayerStartRefresh.hpp"
#include "LevelViewStore.hpp"
#include "AssetEditor.hpp"
#include "ActorEditor.hpp"
#include "AnimEditor.hpp"
#include "GraphEditor.hpp"
#include "BtEditor.hpp"
#include "BtGraphEditor.hpp"
#include "UiLayoutEditor.hpp"
#include "BlendSpaceEditor.hpp"
#include "AnimStateMachineEditor.hpp"
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
#include "SoftBodyPanel.hpp"
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
#endif
#include "EditorEuler.hpp"
#include "SequenceEditor.hpp"
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

// Fluids and Soft-Body are independent of Particles.
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


// Gizmo colours: X red, Y green, Z blue, yellow highlight (UE5 palette).
static const Vec3 kAxisDir[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
static const Vec3 kAxisCol[3] = {{202.0f/255.0f, 38.0f/255.0f, 0.0f}, {103.0f/255.0f, 169.0f/255.0f, 0.0f},
                                  {44.0f/255.0f, 126.0f/255.0f, 237.0f/255.0f}};
static const Vec3 kAxisHi = {1.0f, 1.0f, 0.0f};

// Cube centred at (cx,cy,cz) with half-extent h.
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
// Ground quad in XY plane with half-extent s.
static inline void appendGround(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx, f32 s) {
    const u32 b = static_cast<u32>(v.size());
    v.push_back({-s,-s,0,0,0,1,-0.5f,-0.5f}); v.push_back({s,-s,0,0,0,1,0.5f,-0.5f});
    v.push_back({s,s,0,0,0,1,0.5f,0.5f}); v.push_back({-s,s,0,0,0,1,-0.5f,0.5f});
    idx.push_back(b); idx.push_back(b+1); idx.push_back(b+2); idx.push_back(b); idx.push_back(b+2); idx.push_back(b+3);
}
// UV sphere of radius r, +Z as pole.
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

// Box with independent half-extents (hx,hy,hz), yawed around Z.
static inline void appendBoxYaw(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx,
                          f32 cx, f32 cy, f32 cz, f32 hx, f32 hy, f32 hz, f32 yawDeg) {
    const f32 rad = yawDeg * kDegToRad;
    const f32 cs = std::cos(rad), sn = std::sin(rad);
    // Rotate around Z preserving winding.
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

// Capped cylinder along +Z, flat-shaded.
static inline void appendCylinderZ(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx,
                             f32 cx, f32 cy, f32 cz, f32 radius, f32 halfHeight, u32 segments) {
    for (u32 s = 0; s < segments; ++s) {
        const f32 a0 = kTwoPi * static_cast<f32>(s) / static_cast<f32>(segments);
        const f32 a1 = kTwoPi * static_cast<f32>(s + 1) / static_cast<f32>(segments);
        const f32 x0 = std::cos(a0), y0 = std::sin(a0);
        const f32 x1 = std::cos(a1), y1 = std::sin(a1);
        // Side quad: one normal per face (averaged radial).
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
        // Top/bottom caps: one fan triangle per segment.
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
static inline f32 snapf(f32 v, f32 step) { return step > 0.0f ? std::round(v / step) * step : v; }

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
enum class EditorMode { Select, Landscape, Foliage, Simulate, Animate };
static const char* kEditorModeNames[5] = {"Select", "Landscape", "Foliage", "Simulate", "Animate"};
// Mode descriptions.
static const char* kEditorModeHints[5] = {
    "Pick and transform objects",
    "Sculpt the terrain heightfield",
    "Paint scattered meshes onto the terrain",
    "Run the game in the viewport",
    "Keyframe actors, a camera path and emissive glow on one level timeline",
};
static constexpr int kEditorModeCount = 5;

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

// Editor dark scheme colours and metrics (split from applyDpi for DPI scaling order).
static inline void applyEditorMetrics() {
    ImGuiStyle& s = ImGui::GetStyle();

    // Unscaled; applyDpi applies DPI scaling after.
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

// Editor colour palette (safe to re-run outside widget draw).
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
struct GltfImportSummary { u32 meshesWritten = 0, rigsWritten = 0, clipsWritten = 0,
                                materialsWritten = 0, texturesWritten = 0; };

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
            // RGBA8Unorm (tonemapped composite), NOT device_->backbufferFormat().
            pd.renderTargets[0] = rhi::Format::RGBA8Unorm;
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
            pd.renderTargets[0] = rhi::Format::RGBA8Unorm;   // Tonemapped composite.
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
    void setGiMethodCycle(int everyFrames) { giMethodCycleEvery_ = everyFrames; giMethodCycleCountdown_ = everyFrames; }
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
    void setTaaOverride(bool on) { temporalAaEnabled_ = on; taaFromCli_ = true; }   // --taa / --no-taa
    void setNeuRaaOverride(bool on) { neuraaEnabled_ = on; neuraaFromCli_ = true; } // --neuraa / --no-neuraa

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
    void setConsoleSets(std::vector<std::pair<std::string, std::string>> sets);
    void setMeshHeapDefault(bool on);
    void setLodShareVertices(bool on);
    void setCamTranslate(f32 speedCmPerFrame);
    void setCamWander(f32 amp, f32 speed);        // --cam-wander AMP SPEED
    // --nrd2-capture DIR POSES [HOLD] [HELDOUT_FROM] and --nrd2-oracle grad|grid (NRD2 phase 3 dataset).
    // DIR "default" (or "-") is %LOCALAPPDATA%/AverEngine/nrd2_dataset/<level>.
    void setNrd2Capture(const std::string& dir, u32 poses, u32 hold, u32 heldOutFrom, bool oracleGrid) {
        nrd2CaptureDir_ = dir; nrd2CapturePoses_ = poses; nrd2CaptureHold_ = hold;
        nrd2CaptureHeldOut_ = heldOutFrom; nrd2CaptureGrid_ = oracleGrid;
    }
    // --nrd2-train STEPS [DATASETDIR...] (NRD2 phase 4; no dirs = %LOCALAPPDATA%/AverEngine/nrd2_dataset).
    void setNrd2Train(u32 steps, std::vector<std::string> dirs) { nrd2Session_.requestCli(steps, std::move(dirs)); }
#if AVER_MODULE_SR
    void setNeuRaaCapture(const std::string& dir, u32 count) { neuraaCaptureDir_ = dir; neuraaCaptureCount_ = count; }
#endif
    void setCamera(Vec3 pos, f32 pitchDeg, f32 yawDeg);
    void setScriptsDir(std::string d);            // --scripts <dir>
    void setSpawnTest(std::string cls);      // --spawn-test <ClassName>
    void setUnlitMode(bool on);                                  // --unlit
    // --view-mode lit|unlit|wireframe|rayhit-*|triangles|undenoised (applied at startup).
    void setViewMode(const std::string& mode);
    void setPlayTest();                                       // --play-test
    void setPlayWalk() { defaultPawnWalk_ = true; }          // --play-walk: the no-GameMode default pawn walks
    void startDefaultPawnWalk(bool fromCamera);                // SandboxPlay.cpp: the capsule for a walking default pawn
    void driveDefaultPawnWalk(const Vec3& fwd, const Vec3& right);   // per frame, from the fly block
    void startDefaultPawnFly();                                // SandboxPlay.cpp: the flying default pawn's collider
    void driveDefaultPawnFly(const Vec3& velocity, f32 dt);    // per frame; dt 0 = no drift (hard set)
    void setProjectPath(std::string p);          // <path>.ocproject
    void setStartMode(std::string m);
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
    bool projectDirty_ = false;

    // Auto-saved on edit with 0.5s debounce.
    static constexpr f32 kProjectAutosaveSec = 0.5f;
    f32 projectAutosaveAccum_ = 0.0f;


    // ---- FRAME BUDGET ----
    // Scales GI work to hit target frame time (RENDER.FRAMEBUDGETMS).
    f32  frameBudgetMs_ = 0.0f;       // RENDER.FRAMEBUDGETMS; <= 0 disables
    bool frameBudgetForced_ = false;  // --frame-budget: run even during capture
    // Above unguarded (manifest keys); below guarded on VOXI (controller state).
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
    // Derived Data Cache write-behind budget, mirrored for slider binding.
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

    // Save level and all dirty asset tabs (File > Save All, Ctrl+Shift+S).
    void saveAll();

    // Viewport screenshot to Saved/Screenshots (File > Take Screenshot, F9).
    void requestViewportScreenshot();
    void serviceViewportScreenshot(Engine& e);
    u8  viewportShotState_ = 0;    // 0 idle, 1 requested, 2 capture requested and awaiting the image
    u64 viewportShotFrame_ = 0;    // engine frame the capture was requested on
    u32 viewportShotTries_ = 0;    // frames waited for the image; gives up rather than waiting forever

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

    bool autoCompile_ = false;
    // True when --auto-compile enabled.
    bool autoCompileFromCli_ = false;
    int focusLevelAt_ = 0;
    // Frames before chunk streaming auto-enables.
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
    // World/Project Settings: a combo of declared classes with `flag`, by name (SandboxSettings.cpp).
    bool classPicker(const char* label, std::string& value, int32_t flag, const char* emptyLabel,
                     const char* engineClass, const char* engineLabel);
    // The view request before the editor overrode it (see startPlay).
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
    // Anchor plus set: anchor (sel_/selEntity_) kept as-is, multiSel_ is the rest.
    // INVARIANT: multiSel_ always contains selEntity_ when selected.
    std::vector<scene::Entity> multiSel_;
    // Parallel set for fast O(1) lookup; vector maintains order.
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
    // Outliner row order (multiRange, selectAllInOutliner).
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

    // One undoable edit (transform, create/destroy). Components captured via EntitySnapshot.
    struct EditCmd {
        enum class Kind { Transform, Create, Destroy, CreateObj, DestroyObj, LandscapeStroke, FoliageStroke, Reparent, Material, Rename, RemoveComponent, Visibility, Collision, Animation, Prefab };
        Kind kind = Kind::Transform;
        // Monotonic serial for save points; never reused so undo/redo cross saves correctly.
        u64 serial = 0;
        EditId id = 0;            // a scene entity, through the indirection
        int objIndex = -1;        // or an objects_ index, for the placeholder scene
        EditXform before{}, after{};

        // Other entities moved with anchor as one command (avoids "undo partway" inconsistency). Stored as LOCAL transforms.
        struct AlsoMoved { EditId id = 0; Transform beforeLocal, afterLocal; };
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
#if AVER_MODULE_SCENE
        editor::EntitySnapshot snap;    // scene entity: asset name, persisted id, every other component
#endif
        // Whether to give recreated/pasted entity a body (shape is derived fresh each time).
        bool hadBody = false;
        // Authoring flags (nocollide, snapToGround) not in snap; held in entityCollide_/entitySnapZ_. Fixes destroy-undo bug.
        bool  hadCollide = true;      // the default saveLevel writes for an entity it has no entry for
        bool  hadSnapZ   = false;
        f32   snapZ      = 0.0f;
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
#endif

#if AVER_MODULE_SCENE
        // Removed component type and byte-exact contents (EntitySnapshot::Comp shape).
        editor::EntitySnapshot::Comp removedComponent;
#endif

        // LandscapeStroke: rect diff, not snapshot (512x512 section = 1 MB, touched rect = KB). One entry per stroke, unioned.
        u32 landX0 = 0, landY0 = 0, landX1 = 0, landY1 = 0;   // inclusive sample bounds
        std::vector<f32> landBefore, landAfter;

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
    // One entry per instance this session's brush placed.
    struct FoliagePlaced { Vec3 pos; f32 solidRadiusCm; };
    std::vector<FoliagePlaced> foliagePlaced_;
#endif
#endif

    void undo();

    void redo();

    std::string selectionLabel() const;

    f32 selectedRadius() const;

    // ---- FOUR GROUPS THAT SAT INSIDE `#if AVER_MODULE_SCENE` AND ARE READ FROM OUTSIDE IT ----
    // Plain arithmetic/flags, no scene:: type, each read unconditionally.
    //
    // Preference autosave timer: maybeAutosavePrefs (SandboxAutosave.cpp) runs unguarded. 0.25s debounce.
    // Prefs are cheap to check/write, so the interval is short; dragged slider collapses 60 frames to 4.
    static constexpr f32 kPrefsAutosaveSec = 0.25f;
    f32 prefsAutosaveAccum_ = 0.0f;

    // Deferred autosave answers: drawNotifications writes them, maybeAutosave consumes next tick.
    bool autosavePostponeRequested_ = false;
    bool autosaveRetryRequested_ = false;

    // Create-a-landscape knobs: grid resolution + tile spacing (read by landscape mode panel).
    int landCreateSamples_ = 513;
    f32 landCreateSpacingCm_ = 100.0f;

    // --no-editor-chrome: suppress editor chrome for comparison with AverEngineRuntime.exe.
    bool noEditorChrome_ = false;

    // ---- PlayerStart: where the player spawns in ----
    // Level format had the answer, unread: OcWorldData's hasSpawn/spawnX/Y/Z/Yaw were parsed/written but consulted nowhere.
    // ONE RECORD, SO ONE MARKER: SPAWN is scalar, not a list (two sources of truth is bad).
    // TRANSIENT ENTITY, never pushed to levelEntities_ (like the drone).
    std::vector<std::string> cbDeleteRefs_;
    std::vector<std::string> cbRenameRefs_;   // the same, for the rename dialog
    // Whether the rename dialog repoints what it found (ON by default; repointing is the usual want).
    bool cbRenameRepoint_ = true;

#if AVER_MODULE_SCENE
    scene::Entity playerStart_ = scene::kInvalidEntity;

    // The Outliner row currently being renamed in place, and its edit buffer.
    // Create-a-landscape controls; the shape knobs are landscape_.noiseParams(), shared with the
    // ring generator so a created section and the tiles around it come from one set of numbers.
    // Autosave interval, TEN MINUTES. The old 30s period made the countdown visible; silence
    // would be expected, but now saveLevel doesn't call markLevelSaved, so the timer restarts immediately
    // and a 10s warning on 30s put a countdown visible a third of every minute.
    static constexpr f32 kAutosaveDefaultSec = 600.0f;
    f32 autosaveIntervalSec_ = kAutosaveDefaultSec;
    f32 autosaveAccum_ = 0.0f;

    // Why three states, not a bool: the save is synchronous (onUpdate runs BEFORE onRender), so a "Saving..."
    // notification raised and saved in the same tick would be replaced by "Saved" before buildUI draws it.
    // Pending spends one presented frame saying so, then writes on the tick after.
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
#if AVER_MODULE_SCENE
    // "Add > Point/Spot/Rect Light": a CLight entity in front of the camera, selected, with an undo entry.
    void spawnLightAtCamera(i32 kind);
    // "Add > Decal": a CDecal entity in front of the camera, selected, with an undo entry.
    void spawnDecalAtCamera();
#endif

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
    std::vector<scene::Entity> editorHidden_;
    // Union of selection's world bounds for framing and gizmo anchor.
    bool selectionBounds(Vec3& center, f32& radius) const;
    // Left-drag on empty space draws marquee; release selects intersecting entities (Ctrl adds).
    bool nudgeEditOpen_ = false;
    bool marqueeArmed_ = false;
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

    // --autosave-test latches (outside AVER_WITH_IMGUI since maybeAutosave reads them).
    bool autosaveTestArm_ = false;
    bool autosaveTestLift_ = false;
    bool autosaveTestWarned_ = false;

#if AVER_WITH_IMGUI
    void deleteSelection();

    void copySelection();

    void pasteClipboard();

    void duplicateSelection();

    void runCbMoveTest(const std::string& root);
    std::string cbMoveTestDir_;
    int cbMoveTestFrames_ = 0;

    void runNotifyTest();

    void runPrefsWriteTest();
    int prefsWriteTestFrames_ = 0;
    int  notifyTestFrames_ = 0;
    bool notifyTestLift_ = false;

    // Opening a project with no render settings must not inherit the previous project's settings.
    int projectSwitchFrames_ = 0;
    void runProjectSwitchTest();

    // Validates one .ocgraph end-to-end through the managed GraphValidate export.
    int graphPrintTestFrames_ = 0;
    void runGraphPrintTest();

    // --clear-shader-cache: clears DXIL blob cache. Path empty = user data directory.
    int clearShaderCacheFrames_ = 0;
    std::string clearShaderCacheDir_;
    void runClearShaderCache();

    std::string validateGraphPath_;
    int validateGraphFrames_ = 0;
    void runValidateGraph();

    // --rename-repoint-test: the rename/repoint chain over real project files.
    int renameRepointFrames_ = 0;
    void runRenameRepointTest();

    std::string findRefsPath_;
    int findRefsFrames_ = 0;
    void runFindRefs();

    void runSaveDirtyTest();
    int saveDirtyTestFrames_ = 0;

    void runMultiSelectTest(Engine& eng);
    int multiSelTestFrames_ = 0;

    // Asset picker's assignment helpers: the material case uses INTERNED NAME TOKEN vs path hashes.
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

    // One suggestion row: display (name+description) and insert (bare name).
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

    // One Content Browser root mount.
    struct CbRoot { const char* label; std::string path; bool engine; };
    std::vector<CbRoot> cbRoots() const;

    void cbNavigate(const std::string& dir);

    bool cbCanBack()    const;
    bool cbCanForward() const;
    // Every selected path in the current folder.
    std::vector<std::string> cbSelection_;
    // The paths the current folder is showing, refreshed every frame.
    std::vector<std::string> cbShownPaths_;

    void cbBack();
    void cbForward();

    std::string cbParentDir() const;

    static bool cbIsSourceFile(const std::string& ext);

    const editor::IdeInfo& cbIde() const;

    void cbOpenEntry(const std::string& full, bool isDir);

    void cbInvalidate(const std::string& dir);

    bool cbIsEditable(const std::string& path) const;

    // Drop onto folder, latched at drop time, answered by Copy/Move prompt a frame later.
    std::vector<std::string> cbMoveSources_;
    std::string              cbMoveDest_;
    bool                     cbWantMoveOrCopy_ = false;

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
#endif

    void cbCreateFoliageType();

    void cbCreateNodeGraph();

    void cbCreateBehaviourTree();
    void cbCreateUiLayout();
    void cbCreateBlendSpace();
    void cbCreateStateMachine();

    // Writes a starter .ocinput (Input Scheme) and opens it.
    void cbCreateInputScheme();

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

    // Asset sheet tile for engine asset extension, -1 if none (glyph+colour for type identification).
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


    // True when hay contains needle, ignoring case.
    // ---- Asset picker ----
    // One generic widget over label, id candidates; three fields differ only in sources and id meaning.
    struct AssetChoice { std::string label; u64 id = 0; };

    bool assetPicker(const char* popupId, const std::vector<AssetChoice>& candidates,
                     u64 current, u64* picked);
    char assetPickerFilter_[64] = {};

    // Candidate list valid while last used on previous/current frame; stamp tracks map size.
    struct PickerCands {
        std::vector<AssetChoice> list;
        int   lastFrame = -2;
        usize stamp = 0;
        void drop() { std::vector<AssetChoice>().swap(list); lastFrame = -2; }
    };
    PickerCands pickMeshCands_, pickMaterialCands_, pickEffectCands_;

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
    // Between animSystem().tick and World::flush, every frame.
    void tickSequence(f32 dt);
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
    // One Outliner row, kept across frames (see outlinerSignature for cache validity).
    struct OutlinerRow { scene::Entity ent; scene::Entity par; std::string shown; };

    static std::string lowerCopy(std::string v);

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

    // Outliner row cache and signature tracking for efficient reuse.
    std::vector<OutlinerRow> outlinerRows_;
    std::unordered_map<u32, std::vector<const OutlinerRow*>> outlinerChildren_;
    std::vector<const OutlinerRow*> outlinerRoots_;
    // Entity of each root and indices of roots that have listed children.
    std::vector<scene::Entity> outlinerRootEnts_;
    std::vector<u32> outlinerParentRoots_;
    // Signature the cache was built from, and whether it has been built.
    u64  outlinerCacheSig_ = 0;
    bool outlinerCacheValid_ = false;

    // Cache staleness detection: outlinerSignature (O(1) stamp) and per-slot fingerprints.
    u64  outlinerSignature() const;
    u64  outlinerEditMark() const;
    u64  outlinerSlotSignature(u32 slot) const;
    bool outlinerAuditDiverged(bool full);
    // Rebuilds when cache is stale; no-op otherwise.
    void refreshOutlinerCache();
    // Rebuilds outlinerRows_/outlinerChildren_/outlinerRoots_.
    void rebuildOutlinerCache();
    // True when cache holds no parent/child pair (flat import: every row is a uniform leaf).
    bool outlinerFlat_ = false;
    // One fingerprint per dense slot and next audit cursor position.
    std::vector<u64> outlinerSlotSigs_;
    u32  outlinerAuditCursor_ = 0;
    // outlinerEditMark as of last refresh.
    u64  outlinerEditMark_ = 0;
    // ImGui::GetTime of last rebuild and whether Play is running.
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
#endif

    void buildViewportOverlay();
    // Viewport right-click menu (Play From Here). RMB armed on press, decided on release.
    void drawViewportContextMenu();
    struct ViewportCtx { bool armed = false; bool hasHit = false; Vec3 camPos{}; Vec3 hit{}; };
    ViewportCtx vpCtx_;
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
    u64 resizeCycle_ = 0;
    bool gpuTiming_ = false;
    bool gpuTimingDone_ = false;
    bool gpuTimingStartupReset_ = false;   // GPU average restarted once load work is done.
    u32 resizeStep_ = 0;
    bool lumaSweep_ = false;
    int  lumaSweepStride_ = 1;
    bool lumaSweepPending_ = false;
    u64  lumaSweepFrame_ = 0;
    f32  lumaSweepYaw_ = 0.0f;
    f32  lumaSweepVpX_=0, lumaSweepVpY_=0, lumaSweepVpW_=0, lumaSweepVpH_=0;
    bool fireflyMetric_ = false;
    // Last sampled frame's luminance grid for flicker detection.
    std::vector<f32> fireflyPrevGrid_;
    u32 fireflyPrevW_ = 0, fireflyPrevH_ = 0;
    f32  fireflyMult_ = 8.0f;
    // Default to Move (gizmo ready on open, vs Select which needs a second key press).
    Tool initialTool_ = Tool::Move;
    std::vector<MeshObj> objects_;
    // Selection: sel_ >= 0 is objects_ index, -1 is nothing, -2/-3/-4 sun/sky/post, kSelScene is entity.
    static constexpr int kSelScene = -5;
    int sel_ = 1;
    // AvId (generic "nothing selected" sentinel).
    AvId selEntity_ = kInvalidId;
    bool hideEditorScene_ = false;
    rhi::IDevice* prefsDevice_ = nullptr;
    std::string prefIdeName_;
    bool prefsLoaded_ = false;
    // Set by loadLevel when level supplied CAMERA record; read by onAttach.
    bool levelCameraRestored_ = false;
    editor::AssetEditorHost assetEditors_;
    bool vsyncOffRequested_ = false;
    bool vsyncOnRequested_ = false;
    bool wantMeshReload_ = false;
    // Outliner display names (not scene::World::name which holds asset path).
    std::unordered_map<u32, std::string> entityLabels_;
#if AVER_MODULE_SCENE
    editor::DecalImageResolver decalImages_;  // a decal's image id -> a file under the project's content folder
    editor::DecalGizmo decalGizmo_;           // the selected decal's box and its face handles
    bool showDecals_ = true;                  // Show > Decals: dim boxes for every decal, not just the selected one
    editor::AiDebugOptions aiDebug_;          // Show > AI: sight cones, hearing, path, steering, BT state, cover
    rhi::LineHandle aiDebugMesh_ = 0;         // this frame's AI debug lines (rebuilt every frame an option is on)
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
    Tool tool_ = Tool::Move;   // see initialTool_ for why this is Move and not Select

    // Which mode the viewport is in. UNGUARDED: Landscape is AVER_MODULE_LANDSCAPE-only, but mode switch/dispatch read it unconditionally.
    EditorMode mode_ = EditorMode::Select;
#if AVER_MODULE_LANDSCAPE
    SculptTool sculptTool_ = SculptTool::Raise;
    // 1.0 is the smoothstep the brush always had (see BrushParams::falloff).
    f32 sculptFalloff_ = 1.0f;

    // One entry per .ocfoliage TYPE ASSET, not per mesh. Reusable asset, not a global brush setting (see OcFoliage.hpp).
    struct FoliageSpecies {
        std::string name;         // display name, the file stem
        std::string assetPath;    // the .ocfoliage this came from -- opened by the palette's Edit button
        fmt::OcFoliageData type;  // mesh/material/scale/weight/randomizeYaw/collisionRadiusCm/alignToNormal
        bool         enabled = true;
    };
    std::vector<FoliageSpecies> foliagePalette_;
    // BRUSH-ONLY: tool held vs. what it places; instance randomisation lives per type above.
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
    // A sequence camera showing instead of the free-fly one this frame (set where the camera is
    // pushed; camPos_/yaw_/pitch_ are persisted and stay the editor's own).
    bool viewOverride_ = false;
    Vec3 viewPosOv_{0.0f, 0.0f, 0.0f}, viewFwdOv_{1.0f, 0.0f, 0.0f};
    Vec3 viewPos() const { return viewOverride_ ? viewPosOv_ : camPos_; }
    Vec3 viewForward() const { return viewOverride_ ? viewFwdOv_ : camForward(); }

    // What "1" on the camera-speed dial means, in cm/s (movement integration uses cm/s).
    // DISPLAY scale only: divide by it so default speed reads "1" not "800".
    static constexpr f32 kCamSpeedUnit = 800.0f;
    bool flying_ = false;
    // The pawn viewed in FIRST PERSON this frame, or kInvalidEntity. RESET UNCONDITIONALLY at the top of drivePlayCamera().
    // Used by owner-hide check: a mesh with kMeshRendererHiddenFromOwner skips rasterised draw when this is itself or an ancestor.
#if AVER_MODULE_SCENE
    scene::Entity firstPersonPawn_ = scene::kInvalidEntity;
#endif
    // Latched during the scene pass so the outline draws after every surface is down.
    Mat4 selectionOutline_{}; rhi::MeshHandle selectionMesh_ = 0; bool hasSelection_ = false;
    // Every selected entity's (world transform, mesh id) for this frame's outline pass. Rebuilt and cleared right after drawing.
    std::vector<std::pair<Mat4, u64>> selectionOutlines_;
    // Top-of-atmosphere colour, mirroring rhi::SkyAtmosphere::sunColor's default (white).
    f32 sunColor_[3]={1.0f,1.0f,1.0f}, sunAmbient_=1.0f;
    // Editor's mirror of sky_.zenith/horizon, copied OVER sky_ every frame so rhi::SkyAtmosphere's defaults don't survive frame 1.
    f32 skyZenith_[3]={0.24f,0.45f,0.85f}, skyHorizon_[3]={0.72f,0.83f,0.95f};
    // A tint on the in-scattered sky (white = clear air), and an extinction per cm.
    f32 fogColor_[3]={1.0f,1.0f,1.0f}, fogDensity_=4e-6f;
#if AVER_MODULE_SCENE
    // OPT-IN (Height Fog panel): recomputes fogDensity from the streaming load boundary rather than a fixed density.
    bool matchFogToStreamRadius_ = false;
    f32  fogMatchTargetOpacity_ = 0.9f;   // opacity WANTED at the load boundary itself
#endif
    rhi::PostSettings post_{};
    // What this editor last handed the device, so per-frame push can tell when something ELSE (console post.* vars) changed it.
    rhi::PostSettings postPushed_{};
    bool postPushedValid_ = false;
    // Which post values came from argv, so a stored preference cannot outrank a flag the caller typed.
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
    // The baked grid and the line mesh drawn from it. Rebuilt on every bake/load; mesh destruction needed to avoid upload buffer leak.
    fmt::OcNavData  nav_;
    rhi::LineHandle navMesh_=0;
    bool showNav_=false;
    bool navRegionColours_=true;
    // --bake-nav: bake once at startup, then carry on. Deferred to a frame because bake reads PHYSICS BODIES, built by applyProject at init.
    bool navBakeOnStart_=false;
    bool navBakeDone_=false;
    bool navLoadPending_=false;
    f32  navBakeCell_=50.0f;
#endif

    // ---- the state belonging to the three non-navigation features above ----
    // Collider overlay: the world-space AABB of every physics body. TOGGLE UNGUARDED; MESH guarded on AVER_MODULE_PHYSICS.
    // Toggle persisted through editor.ini (runs in every config); mesh genuinely built from bodies.
    bool showColliders_=false;
#if AVER_MODULE_PHYSICS
    // Two meshes: static bodies in colliderMesh_, kinematic/dynamic in colliderMoverMesh_.
    rhi::LineHandle colliderMesh_=0;
    rhi::LineHandle colliderMoverMesh_=0;
    // What colliderMesh_ was built from, so rebuildColliderOverlay can skip unchanged frames.
    // AllKnown: every STATIC body is one the editor tracks, so the sig covers all change paths.
    bool colliderOverlayBuilt_ = false;
    bool colliderOverlayAllKnown_ = false;
    u64  colliderOverlaySig_ = 0;
    u64  colliderRev_ = 0;
    // The moving mesh's state: handles, tracking, post-play refresh, and AABBs for rebuild comparison.
    std::vector<int32_t> colliderMovers_;
    std::vector<f32> colliderMoverBoxes_, colliderMoverBoxesNext_;
    std::vector<rhi::LineVertex> colliderMoverLines_;
    bool colliderMoversTracked_ = false;
    bool colliderMoversWereLive_ = false;
    // The static audit: the handles colliderMesh_ drew, with the AABB each was drawn at. In Play, a rotating slice is re-read each frame.
    std::vector<int32_t> colliderStatics_;
    std::vector<f32> colliderStaticBoxes_;
    usize colliderAuditCursor_ = 0;
#endif
    // Frame time and the Play-only CPU phases, fed by onUpdate's begin()/end() brackets and folded in the status bar.
    editor::PlayProfile playProf_;
#if AVER_WITH_IMGUI
    // The profiler panel (Window > GPU Profiler): GPU pass table and frame time/Play CPU phases.
    bool showProfiler_=false;
    // The status bar's VRAM readout: the last videoMemory() answer, polled twice a second, and over-budget warning flag.
    rhi::VideoMemoryInfo vram_{};
    f32  vramPollS_ = 0.0f;
    bool vramOverLogged_ = false;
    // Window > Neural Visualiser. Choices applied every frame by onUpdate. Closing the window doesn't turn them off (its "Off" entries do).
    bool showNeuralViz_ = false;
    int  neurafiVizMode_ = 0;          // neurafi::Visualisation
    f32  neurafiVizOpacity_ = 0.85f;
    f32  neurafiVizScalePx_ = 0.5f;    // path bend at full heat
    bool neurafiShowGeneratedOnly_ = false;
    int  neuracViewMode_ = 0;          // VoxiRenderer::setNeuRaCView's mode
    bool neuracViewGrid_ = false;
    // The References panel. refPanelScanned_ distinguishes "opened but never asked" from "asked and found nothing".
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
    // Rotation drag accumulators for snap working on total swept angle rather than per-frame delta.
    f32 rotDragDeg_=0.0f, rotAppliedDeg_=0.0f;
    f32 prevMouseX_=0, prevMouseY_=0;
    // snapping (off by default; toggled from the toolbar carets)
    bool snapMove_=false, snapRot_=false, snapScale_=false;
    f32 moveSnap_=1.0f, rotSnap_=15.0f, scaleSnap_=0.25f;
    // frame state
    f32 dpi_=1.0f;
#if AVER_WITH_IMGUI
    ImFont* fontMedium_=nullptr; // Roboto Medium, for the menu bar; null if only the fallback loaded
    // Editor's Dear ImGui backend, installed into the device non-owning. Deleted late in onShutdown for device's uiShutdown().
    std::unique_ptr<rhi::d3d12::IUiBackend> uiBackend_;
#endif
#if AVER_WITH_IMGUI_VULKAN
    // The Vulkan backend, held on same terms as the D3D12 one above for teardown ordering.
    std::unique_ptr<rhi::vkb::IUiBackend> uiBackendVk_;
#endif
    // 3D viewport rect, in backbuffer pixels. Latched by buildUI, consumed the next frame.
    f32 vpX_=0, vpY_=0, vpW_=1600, vpH_=900;
    bool dockBuilt_=false;   // false = build the default layout on the next frame
    // Set by View > Reset Layout so the builder overrides a layout restored from the ini.
    bool dockResetRequested_=false;
    // Warn ONCE about a missing icon font (not once per DPI change).
    bool iconWarned_=false;
    bool showProjectSettings_=false; // Edit > Project Settings window
    bool showWorldSettings_=false;   // Window > World Settings (per-LEVEL settings)
    // Window > World Outliner / Details. Default ON: these are the editor's two primary panels.
    bool showOutliner_=true;
    bool showDetails_=true;
    // Window > Soft Body (plastic): the plastic-material test bar. Not persisted.
    bool showSoftBody_=false;
    editor::SoftBodyPanelState softBody_;
    // Viewport Show flags. Both default ON.
    bool showStaticMeshes_=true;
    bool showAtmosphere_=true;
    // File > Save Level As...
    bool unlit_=false;                 // View mode: Unlit (no shading, authored colour only)
    bool neuraaDebugView_=false;       // View mode: Edge Classes (NeuRAA); forces ray-driven
    bool showAbout_=false;             // Help > About
    bool wantSaveLevelAs_=false;
    char saveLevelAsName_[128]={};
    std::string saveLevelAsError_;
    bool showEditorPrefs_=false;     // Edit > Editor Preferences window
    bool scrollPrefsToKeybinds_=false;   // --scroll-prefs-to-keybinds, one-shot
    int  settingsPage_=1;            // 0 Description, 1 Rendering>General, 2 >GI, 3 >Ray Tracing, 4 >Path Tracing
    int  focusVoxi_=0;               // --project-settings: frames left to force the window open
    int  msaaOverride_=0;            // --msaa N: apply a sample count at startup
    // --depth-prepass: depth-only pass ahead of ordinary opaque walk, OFF (default) never calls setDepthPrepassEnabled/drawMeshDepthPrepass.
    bool depthPrepassOverride_ = false;
    // --gbuffer: debug view override, OR'd together every frame with the view-mode dropdown.
    bool gbufferOverride_ = false;
    // --gbuffer-debug velocity|viewz|normals: which channel (if any) GBufferDebugFeature draws over the 3D viewport.
    GBufferDebugFeature::Mode gbufferDebugView_ = GBufferDebugFeature::Mode::Off;
    // Registered once in onInit, unregistered in onShutdown. Never rebuilt: one instance for the whole run.
    GBufferDebugFeature gbufferDebugFeature_;
    bool gbufferDebugAttached_ = false;
#if AVER_WITH_SYNAPSE_AI && AVER_MODULE_SYNAPSE_GPU
    // The GPU crowd-avoidance backend, installed behind the Synapse ABI's one SynapseAi (game::installCrowdGpu).
    synapse::GpuCrowdBackend crowdGpu_;
    bool crowdGpuAttached_ = false;
#endif
    // Window > Neural Visualiser's NeuraFI overlay. Registered beside gbufferDebugFeature_, for the run.
    NeuraFiVizFeature neurafiViz_;
    // --occlusion-cull: hierarchical-Z two-pass box culling (modules/occlusion). OFF (default) never calls occluder_.
    // UNGUARDED on this side of the #if, a MANIFEST MIRROR (RENDER.OCCLUSIONCULL round-trips through it).
    bool occlusionCullEnabled_ = false;
// AND AVER_MODULE_SCENE, not just OCCLUSION -- scene::Entity keys on these members.
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
    bool occlusionCullForceOff_ = false;   // --no-occlusion-cull: see setOcclusionCullForceOff's own comment
    // --occlusion-waitidle / --no-occlusion-waitidle: see setOcclusionDebugForceWaitIdle. DEFAULTS TRUE.
    bool occlusionDebugForceWaitIdleArg_ = true;
    // NON-owning: SandboxApp owns the one instance for the run and destroys it in onShutdown.
    aver::occlusion::IOcclusionCuller* occluder_ = nullptr;
    // Per-entity "the pyramid could not prove this hidden, as of last test" bit, carried across frames.
    std::unordered_map<scene::Entity, bool> occlusionVisible_;
    // This frame's reordering of [0, w.count()) so every PASS-1 (assumed-visible) index precedes PASS-2.
    std::vector<u32> occlusionOrder_;
    // This frame's world AABBs, collected in a pre-walk: the culler needs ALL of them in one call.
    std::vector<aver::occlusion::Aabb> occlusionBoxes_;
    std::vector<scene::Entity> occlusionBoxEntities_;
    std::vector<u8> occlusionResults_;
    // Diagnostics: accumulated since the process started, and the frame count they cover.
    u64  occlusionCulledAccum_ = 0, occlusionTestedAccum_ = 0;
    u32  occlusionReportFrames_ = 0;
    bool occlusionWasVisible(scene::Entity e) const;
    // MOTION-SAFE CULLING: the camera basis testBatch()'s answer was computed from one-frame-stale camera (see Occlusion.hpp's TWO-PASS).
    Vec3 occlusionBasisCamPos_{0.0f, 0.0f, 0.0f};
    Vec3 occlusionBasisForward_{1.0f, 0.0f, 0.0f};
    bool occlusionBasisValid_ = false;
    // F7: the scene's sub-rect (target pixels) that produced THIS basis's pyramid. A dock-layout drag is a discontinuity like a teleport.
    f32 occlusionBasisRect_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // F7's once-per-change diagnostic: last rect/pyramid size the "[Occlusion] testing..." line printed for.
    f32 occlusionLoggedRect_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    u32 occlusionLoggedPyramidW_ = 0xFFFFFFFFu, occlusionLoggedPyramidH_ = 0xFFFFFFFFu;
    // F8: has the "[Occlusion] idle: ..." line already fired for the CURRENT idle streak.
    bool occlusionIdleLogged_ = false;
    // 3B: this frame's copy of the two EditorConsole.hpp slots, reasserted every frame.
    bool occlusionShowCulled_ = false;
    bool occlusionCullUnderSuppression_ = false;
    // Diagnostics for the staleness detector: frames it fired on, and whether the one-time warning already fired.
    u64  occlusionStaleReadbacks_ = 0;
    bool occlusionStaleWarnedOnce_ = false;
#endif
    // All three use -1 for "flag not given", NOT 0 (0 = Quality::Off, must be expressible).
    int  giOverride_=-1;             // --gi [tier]: GI quality to apply at startup
    bool giForceOff_=false;          // --no-gi: force it off, whatever the default is
    int  rtOverride_=-1;             // --rt [tier]: ray tracing quality at startup
    int  ptOverride_=-1;             // --pt [tier]: PATH tracing quality at startup
    bool rtForceOff_=false;          // --no-rt: force it off, whatever the default is
    int  rtRaysOverride_=0;          // --rt-rays N: sun occlusion rays per pixel (0 = flag not given)
    // --gi-sky-occlusion-rays N: AMBIENT sky-visibility rays per pixel. -1 = flag not given (0 is a real value).
    int  giSkyOccRaysOverride_=-1;
    int  giSkyOccTileOverride_=-1;   // --gi-sky-occlusion-tile N
    // --sky-light N. NEGATIVE means absent: 0 is a real, meaningful request (no sky ambient).
    f32  skyLightOverride_=-1.0f;
    // --gi-intensity F. Negative means absent; 0 is a real request (bounce off, direct only).
    f32  giIntensityOverride_=-1.0f;
    // --sky-physical / --sky-authored: -1 absent, 0 authored, 1 physical. Sun elevation in degrees (-999 when absent).
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
    // --gi-mode N: indirect-diffuse estimator (0 = voxel cones, 1 = ReSTIR GI). Sentinel is -1 (0 is a real value).
    int  giModeOverride_=-1;
    // --restir-visibility none|reconstructed|half|full (Settings::giRestirVisibility). Sentinel is -1 (0 is a real value).
    int  restirVisibilityOverride_=-1;
    int  denoiserOverride_=-1;       // --denoiser 0|1|2: -1 is "flag not given"; see setDenoiser
    f32  renderScaleOverride_=1.0f;  // --render-scale F: scene render resolution as a fraction of present, clamped [0.25,1]
#if AVER_MODULE_SR
    // --aversr LEVEL / the render-settings quality combo. Off (default) = no AverSR.
    aver::sr::Quality averSrQuality_ = aver::sr::Quality::Off;
    // True when --aversr set the value above, so loadEditorPreferences must not let a stored preference overwrite the CLI flag.
    bool averSrFromCli_ = false;
    bool averSrCliAuto_ = false;      // --aversr auto: CLI wins even though auto has no single level to pin.
    int  averSrCliLevel_ = -1;        // --aversr LEVEL (not auto): the pinned level.
    // User's Display preference, superset of averSrQuality_ (Auto and Manual besides the four named levels).
    editor::AverSrChoice averSrChoice_ = editor::AverSrChoice::Auto;
    // Set once when a stored display.renderScalePending cookie is found still armed at load; forces Off for the REST OF THIS SESSION.
    bool averSrCookieTripped_ = false;
    // Set true when migrateAverSrChoice ran on a genuinely ABSENT display.aversrChoice and landed on Auto.
    bool averSrMigrationNoteArmed_ = false;
    // Set the first time updateAverSrAuto is about to apply a non-Off level THIS SESSION.
    bool averSrArmedNonOffOnce_ = false;
    // Fires the "[AverSR] ... scene WxH -> present WxH" line exactly once per process.
    bool averSrStartupLogged_ = false;
    // Why the CURRENTLY APPLIED level is what it is -- Auto/Manifest/User/Cli/ForcedOff.
#if AVER_MODULE_VOXI
    voxi::AverSrSource averSrSource_ = voxi::AverSrSource::Auto;
#endif
    // Project's own AverSR default combo's live edit state. Unconditionally reset to -1 on project open (not inherit from previous project).
    int  averSrProjectDefault_ = -1;
    // FSR 1 (EASU + RCAS). Built whenever the scene is scaled (any AverSR level or a manual render
    // scale) or edge AA is on; dropped, after the device lets go of it, when none is (applyUpscalerSlot).
    std::unique_ptr<aver::sr::FsrUpscaler> averSrUpscaler_;
    // Temporal AA (TAAU + RCAS): when on, it takes the slot instead of FSR, at any render scale.
    std::unique_ptr<aver::sr::TemporalUpscaler> taaUpscaler_;
    // NeuRAA wraps whichever upscaler holds the slot. Phase 1: only while its debug view is selected.
    std::unique_ptr<aver::sr::NeuRaa> neuraa_;
    std::string neuraaCaptureDir_;          // --neuraa-capture DIR COUNT (training data)
    u32  neuraaCaptureCount_ = 0;
    bool neuraaCaptureStarted_ = false;
    bool temporalAaEnabled_ = true;   // the project's RENDER.TAA (unstated = on); --no-taa / --taa win over it
    bool taaFromCli_ = false;
    bool neuraaEnabled_ = false;      // the project's RENDER.NEURAA (unstated = off)
    bool neuraaFromCli_ = false;
    // Edge AA (FXAA-class) in FSR's first pass: --edge-aa or Display > Edge anti-aliasing.
    bool edgeAaEnabled_ = false;
    f32  fsrSharpness_ = 0.2f;   // RCAS stops (0 = sharpest); Display > Sharpening

    // ---- frame interpolation (docs/rendering/NEURAFI.md) ----
    // Who decides, highest first: --frame-interp, project RENDER.FRAMEINTERP, Editor Preference (off by default).
    int  frameInterpCli_ = -1;
    bool frameInterpWhileEditing_ = false;   // Editor Preferences, display.frameInterpWhileEditing
    u32  msaaPushed_ = 0;                    // sample count last given to the device (onUpdate)
    // The gather's path and in-engine training (Editor Preferences display.frameInterpTrajectory / display.frameInterpTrain).
    int  frameInterpTrajectory_ = 2;         // neurafi::Trajectory (0 straight, 1 quadratic, 2 learned)
    bool frameInterpTrain_ = false;
    int  frameInterpTrajectoryCli_ = -1;     // --frame-interp-trajectory; -1 = not given
    bool frameInterpTrainCli_ = false;       // --frame-interp-train
    // The status bar's frame rate counts interpolated frames or real frames only; chosen by clicking it.
    bool fpsCountsInterpolated_ = true;
    std::unique_ptr<aver::neurafi::NeuraFI> frameInterpolator_;
    // Decides and pushes this frame's frame-interperation state; returns whether it is wanted (the G-buffer must then be on).
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
    u64  camWanderHeld_=0;           // frames the wander was paused (NeuRAA capture holding a pose)
    std::string nrd2CaptureDir_;     // --nrd2-capture (see setNrd2Capture)
    u32  nrd2CapturePoses_=0, nrd2CaptureHold_=256, nrd2CaptureHeldOut_=~0u;
    bool nrd2CaptureGrid_=false, nrd2CaptureStarted_=false;
    Vec3 camWanderBasePos_{};
    f32  camWanderBaseYaw_=0.0f, camWanderBasePitch_=0.0f;
    // --mesh-heap default|upload (W4): false (default) = static mesh vertex/index buffers on the Upload heap.
    // true moves them to the Default heap (see rhi::IDevice::setStaticMeshHeapDefault).
    bool meshHeapDefault_ = false;
    // --lod-share-vertices 0|1 (W11): false (default) = every coarser LOD gets its own vertex buffer,
    // duplicating LOD0's untouched data. true shares LOD0's buffer via createMeshSharingVertices,
    // falling back to independent buffers on refusal (unsupported backend, compute-written vertices).
    // Read inside loadProjectMeshes' LOD-ladder loop, once per mesh at load time.
    bool lodShareVertices_ = false;

    Vec3 camPosOverride_{};
    f32  pitchOverride_=0.0f, yawOverride_=0.0f;   // radians, converted in setCamera
    bool useWarp_=false;             // --warp: run on the D3D12 software rasteriser
    // The engine, for the log sink to write the startup splash through. Set at the top of onInit.
    Engine* engineForSplash_ = nullptr;
    std::thread::id mainThreadId_{};
    bool windowedOverride_=false;    // --windowed: kept so the flag still parses; windowed is the default now
    bool fullscreenOverride_=false;  // --fullscreen: opt an interactive run INTO borderless fullscreen
    std::string backendName_;   // --backend: which RHI backend to ask for first
    // RENDER.BACKEND as the OPEN PROJECT states it. Separate from backendName_ (what this run launched with).
    std::string projectBackend_;
    std::string runningBackend_ = "?";   // what the device actually came back as, latched at init
    bool debugLayer_=false;          // --debug-layer: validate every graphics call (a real per-call tax)
    std::string scriptsDir_;         // --scripts <dir>: where to look for user script assemblies
    std::string spawnTestClass_;     // --spawn-test <ClassName>: headless actor-loop test trigger
    bool spawnTestDone_=false;       // the spawn is one-shot, done on the first frame scripts are ready
    int32_t spawnTestEntity_=0;      // the spawned test entity, destroyed a few frames later
    int spawnTestFrames_=0;          // frames since the test spawn, so the destroy is one-shot too
    // The open drawer's CURRENT animated height in pixels, 0 when closed. Published to avoid viewport hint drift.
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
    bool projectOpenPending_=false;   // browser chose a project; applied at the top of onUpdate
    // Whether THIS launch's own command line was eligible to register as the single-instance primary.
    bool singleInstanceEligible_ = false;
    // ONE PENDING OPEN, whatever asked for it: File > Open Level's picker, a Content Browser double-click, or a path forwarded from a second launch.
    // Funnelled through one request so the unsaved-changes guard and the class-placement spawn happen once.
    std::string pendingOpenPath_;
    std::string pendingOpenWhy_;      // how it was asked for, for the modal's own sentence
    // The modal shown when a pending open would discard unsaved work. Separate from exitPrompt_.
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
    // The tile counts, named once each to avoid silent shifts from mismatched values.
    static constexpr int kFileIconTiles   = 4;
    static constexpr int kFolderIconTiles = 2;
    static constexpr int kAssetIconTiles  = 4;   // anim, skeleton, mesh, graph
    // Tiles are packed into one int so a cached DirListing entry stays one field.
    static constexpr int kAssetTileBase   = 100;
    std::unordered_map<std::string, std::pair<std::filesystem::file_time_type, int>> fileIconCache_;
    // A .ocgraph's DOMAIN record, read once and kept against the file's mtime (same cache shape as fileIconCache_, rebuilt with the listing).
    std::unordered_map<std::string, std::pair<std::filesystem::file_time_type, editor::GraphAssetFamily>>
        graphDomainCache_;
    std::unordered_map<std::string, DirListing> dirCache_;
    int frameNo_ = 0;                                      // bumped once per UI frame; the cache freshness clock
    rhi::TextureHandle compileIconTexture_=0;   // the Compile C# status sprite sheet (3 tiles)
    u64 compileIconUiId_=0;
    u64 logoUiId_=0;
    f32 logoAspect_=1.0f;
    editor::ToolsMenu tools_;
    editor::Nrd2Session nrd2Session_;   // Tools > Train Neural Denoiser, --nrd2-train
    editor::ShaderWarmup shaderWarmup_; // Sandbox.exe --warm-shaders after a project opens
    // "Preparing <level>" after the loading screen while ray-tracing structures still build (SandboxRender.cpp).
    bool levelPrepArmed_ = false;
    u64 levelPrepToast_ = 0;
    std::chrono::steady_clock::time_point levelPrepT0_{};
    // "Compiling shaders N of M" while the renderer's pipelines build off the main thread (black viewport).
    u64 shaderBuildToast_ = 0;
    std::chrono::steady_clock::time_point shaderBuildT0_{};
    bool nrd2MenuWired_ = false;
    // Gizmo coordinate space. Honoured by drawGizmo, pickAxis, applyMove and applyRotate; SCALE tool always uses local.
    bool worldSpace_=true;
    bool giDebugView_=false; Vec3 giCenter_{0,0,300}; f32 giExtent_=1200.0f;   // cm
    bool giConeTraceOff_=false;   // --no-gi-cone: see setGiConeTraceOff's own comment
    // --view-mode undenoised / the dropdown's independent "Undenoised" toggle: forces a bundle of existing runtime knobs off.
    // NOT PERSISTED: reasserted every frame.
    bool undenoised_=false;
    // True when --view-mode or --unlit set the view for this run: stored viewport.wireframe/viewport.unlit don't override it on load.
    bool viewModeFromCli_=false;
#if AVER_MODULE_VOXI
    voxi::VoxiRenderer voxiRenderer_;
    editor::SceneLightFeed sceneLightFeed_;   // CLight entities -> Voxi lamps + path-tracer lights, per frame
    game::SceneDecalFeed sceneDecalFeed_;     // CDecal entities -> Voxi's projected-decal list, per frame
    bool voxiAttached_=false;
    // Viewport's ray-hit/triangles debug view (Ray Hit: Instances/Materials/Distance, Triangles), or None. NOT PERSISTED: reasserted every frame.
    voxi::VoxiRenderer::ViewDebug debugView_ = voxi::VoxiRenderer::ViewDebug::None;
    // Last frame's EFFECTIVE voxi::Settings::rtRenderMode, after onUpdate's auto-switch. Lets onUpdate tell an actual mode transition from an ordinary frame.
    i32 lastEffectiveRtRenderMode_ = -1;
#endif
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // Registration is non-owning: particleRenderer_ must outlive the device, torn down in onShutdown.
    particles::ParticleRenderer particleRenderer_;
    bool particlesAttached_=false;
#endif

    // Water is an INDEPENDENT module from Particles. --water <heightCm> is the opt-in.
#if AVER_MODULE_FLUIDS
    // The level's analytic surface and simulated volumes, shared with the runtime.
    game::GameWater water_;
#endif
    // These two stay OUTSIDE the guard: the flag is parsed either way.
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

    // NEEDS THE SCENE AS WELL AS SCRIPTING: parameter is a scene::Entity, sink is aver::anim::AnimNotifyFn.
#if AVER_MODULE_SCENE
    static void animNotify(scene::Entity e, const char* name, void* user);
#endif

    scripting::ScriptHost scripts_;
#endif
    // The retained game UI's renderer. Heap-owned because create() may decline.
    aver::render::ui::UiRenderer* gameUi_ = nullptr;
    bool skinTest_ = false;       // --skin-test: GPU skinning against its CPU reference, then exit
    std::unique_ptr<aver::render::SkinSelfTest> skinSelfTest_;
    // VERDICTS LATCHED OUT OF THE TWO TESTS ABOVE: onShutdown destroys them before exitCode() is read. -1 = never asked, 0 = passed, 1 = failed.
    int skinSelfTestExit_ = -1;
    int skinDrawExit_     = -1;
    bool skinDrawTest_ = false;   // --skin-draw-test: does the RASTERISER read the skinned buffer
    // BESIDE skinDraw_, outside the scene guard: ThumbnailCache (sandbox/src/ThumbnailCache.hpp) stands on the RHI and ActorPreview only.
    // The content browser already treats that as settled.
    aver::editor::ThumbnailCache thumbnails_;
    std::unique_ptr<aver::editor::SkinDrawTest> skinDraw_;
#if AVER_MODULE_SCENE
    // The scene join: gives every entity with a CSkeletalMesh its own posed mesh. Null when the skinning shader would not compile.
    std::unique_ptr<aver::render::SkinnedScene> skinnedScene_;
#if AVER_MODULE_RENDER_SOFTBODY
    // Beside skinnedScene_: a per-entity vertex source that the scene pass substitutes for the authored mesh.
    std::unique_ptr<aver::render::SoftBodyScene> softBodyScene_;
#endif
#endif
    std::string skinSceneDir_;
    std::string shaderSourceDir_;                 // --shader-source <dir>, empty = off
    aver::DirectoryWatcher shaderWatch_;    // --skin-scene-test <dir>: where the cooked rig lives
    std::unique_ptr<aver::editor::SkinSceneTest> skinScene_;
    bool particleTest_ = false;   // --particle-test: a dust cloud straddling an opaque occluder.
    bool noParticleGi_ = false;   // --no-particle-gi: see setNoParticleGi's own comment
    // VERIFICATION-ONLY: --particle-stress <N> <M> spawns N grid-arranged emitters capped at M particles.
    int  particleStressEmitters_ = 0;
    int  particleStressMaxParticles_ = 0;
    bool particleStressSecondEmitter_ = false;   // --particle-stress2: diag, see its own comment
    // Direct CPU wall-clock timing around particles::particleSystem().tick(). Accumulated every frame, printed once at shutdown.
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
    // --pt-scene: the path tracer pointed at the real scene. ptSceneViewWantEnabled_ is the flag that matters.
    std::unique_ptr<aver::pt::PtSceneView> ptSceneView_;
    // REQUESTED state (--pt-scene at startup, or the settings-page Quality combo later); ptSceneView_ != nullptr is the ACTUAL one.
    bool ptSceneViewWantEnabled_ = false;
    // --pt-scene WAS GIVEN ON THE COMMAND LINE. Sticky for the session, separate from the want flag above.
    bool ptSceneViewFromCli_ = false;
    bool ptSceneViewUnavailable_ = false;   // init() refused once this session -- stop re-asking
    // A1: true while syncPtSceneView() holds ptSceneViewWantEnabled_ down because ray-driven primary visibility is painting the scene.
    bool ptSceneViewSuppressedByRayDriven_ = false;
    // Wireframe/a G-buffer debug view withdrew the path-traced view's want; remembered so leaving the view mode gives it back.
    bool ptSceneViewSuppressedByViewMode_ = false;
    // --pt-scene-toggle-on/--pt-scene-toggle-off [N]: VERIFICATION ONLY. Simulates flipping the Path Tracing Quality combo N frames into a bounded run.
    int ptQualityRampEvery_ = 0;
    int ptQualityRampCountdown_ = 0;
    int giMethodCycleEvery_ = 0;       // --gi-method-cycle N
    int giMethodCycleCountdown_ = 0;
    int ptSceneToggleOnAutoFrames_ = 0;
    int ptSceneToggleOffAutoFrames_ = 0;
    // --sun-set-at N ELEV AZIM: VERIFICATION ONLY. Simulates dragging the Directional Light panel's sliders, N frames into a --frames run.
    int sunSetAtFrames_ = 0;
    f32 sunSetElevDeg_ = 0.0f, sunSetAzimDeg_ = 0.0f;
    // --sun-sweep START DEG: the DRAG, not the jump -- turns the sun's azimuth by sunSweepDeg_ every frame for the rest of the run.
    int sunSweepFrames_ = 0;
    f32 sunSweepDeg_ = 0.0f;
    // --sun-sweep-frames N: the drag LETS GO after N turns (0 = never), and the final angles are logged.
    int sunSweepTurnsLeft_ = 0;
    int giHistoryResetAtFrames_ = 0;
    // True while a stored non-unity render scale is on trial this session.
    bool renderScaleCookieArmed_ = false;
#if AVER_MODULE_SR
    // --aversr-cycle [N]: drive the on-then-off transition. Verification-only; 0 means never.
    int averSrCycleFrames_ = 0;
#endif
    std::unique_ptr<aver::editor::ReflTest> refl_;
    int  reflBeaconIndex_ = -1;   // which objects_ entry the schedule shows and hides
    rhi::MeshHandle unitCubeMesh_ = 0;   // the editor's own unit cube, half-extent 1
    bool showUiDemo_ = false;
    std::string matSaveStatus_;   // what the last 'Save to C#' did
    unsigned centralDock_ = 0;    // the dock node an opened asset editor lands in
    bool levelFocused_ = true;    // the Level tab holds the keyboard
    // The two panels a selection is also made from, so the edit verbs reach a selection made there.
    bool outlinerFocused_ = false;
    // A row asked to be deleted; answered after the tree walk (drawOutlinerRow). GUARDED: type is scene::Entity.
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
    std::string lastOpenAssetPath_;  // last path --open-asset opened; openAsset_ itself is cleared once consumed.
    std::string graphSelectNode_;
    std::string graphTab_; // --graph-select <nodeId>; see setGraphSelectNode's own comment

    editor::ProjectUpgrade pendingUpgrade_;
    bool        upgradeAsked_ = false;
    bool        exitPrompt_ = false;      // the unsaved-changes modal is up
    std::string exitPromptError_;         // why a "Save all" attempt failed
    // Every PCGVOLUME the loaded level carried, kept verbatim so a save cannot drop them.
    std::vector<fmt::OcPcgVolume> levelPcgVolumes_;
    // The loaded level's own header, placements and PCG volumes stripped. Starting from what the file said, overwriting only what the editor owns.
    // ---- the 3D-viewport icon renderer, and the Player Start marker it draws ----
    // viewportIconsReady_ is the ONE flag the render walk consults to skip the Player Start's cube (false unless the feature AND its texture both came up).
    editor::ViewportIconRenderer viewportIcons_;
    bool viewportIconsReady_ = false;
    editor::ViewportIconRenderer::IconHandle playerStartIcon_ = editor::ViewportIconRenderer::kNoIcon;
    // The capsule/arrow line meshes, built once and placed with a world matrix per frame. Independent of viewportIconsReady_ (that flag gates only the SPRITE).
    rhi::LineHandle playerStartCapsule_ = 0, playerStartCapsuleSel_ = 0, playerStartArrow_ = 0;
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
    f32  uiDemoHealth_ = 0.72f, uiDemoStamina_ = 0.44f, uiDemoScroll_ = 0.0f, uiDemoClock_ = 0.0f;
    // Game UI font. Invalid font (missing file) draws nothing.
    ui::UiFont uiFont_;
    rhi::TextureHandle uiFontTexture_ = 0;

    rhi::MeshHandle cubeMesh_=0; u32 cubeTris_=0; int spawnCount_=0;
    // Content index, asset resolvers, mesh registry, materials (shared with runtime).
    game::GameContent content_;
    // Loaded level: parsing, placements, environment, PCG (runtime GameLevel). Entities list is editor-local.
    game::GameLevel level_;
#if AVER_MODULE_PBR
    // Mesh id -> cached outline line mesh (0 = no outline).
    std::unordered_map<u64, rhi::LineHandle> selOutlineLines_;
#endif
    // Selected mesh asset id (distinct from GPU handle). Cleared by placeholder Floor/Cube loop.
    u64 selectionMeshId_ = 0;

    // Landscape CLI override path. Must exist unconditionally for command-line parsing.
    std::string landscapeCliOverride_;
#if AVER_MODULE_LANDSCAPE
    // Terrain section: render, collision, authoring (shared with runtime).
    game::GameLandscape landscape_;

    // Sculpt tool state: radius/strength shared across all brush modes.
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

    // Camera view persistence per level (LevelViewStore.hpp, LevelViews.ini).
    void storeLevelView();
    bool lookupLevelView(const std::string& levelPath, editor::LevelView& out) const;
    // Apply view with mouse-look and wheel clamps.
    void applyLevelView(const editor::LevelView& v);
    std::string levelViewsPath() const;
    // Editor view saved before Play started (restored on stop).
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

    // Snapshot of transforms/visibility/spawned entities at Play start; restored on Stop (not a full reload).
    bool defaultPawnPlay_ = false;  // True while engine's default GameMode is active.
    struct PlaySavedTransform { scene::Entity e; Transform xf; bool visible = true; };
    std::vector<PlaySavedTransform> playWorldSnapshot_;
    bool playWorldCaptured_ = false;
    // What moved during session (Voxi GI excludes it). Started in capturePlayWorld, ended in stopPlay.
    game::PlayMobility playMobility_;
#if AVER_MODULE_SCENE
    // The level's sequence (Animate mode, Play). Its camera and emissive reach the frame through
    // viewOverride_ and DrawWorldOptions::emissiveScale.
    editor::SequenceEditor seqEditor_;
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS
    // Vehicles as physics cars while Play runs. Built in capturePlayWorld, ended in stopPlay.
    world::VehicleSystem vehicles_;
#endif

    std::vector<scene::Entity> levelEntities_;
    bool hasLevelSun_ = false;
    bool hasLevelSky_ = false;
    bool hasLevelFog_ = false;
    // True if level record mentioned clouds (not just fmt::OcWorldEnv::hasClouds).
    bool hasLevelClouds_ = false;

#if AVER_MODULE_FRAMEWORK
    // Class placements: level_.classPlacements() has the list; spawnClassPlacements fills this.
    // Each instance carries its placement id explicitly, not by position.
    using ClassInstance = editor::LevelClassInstance;
    std::vector<ClassInstance> levelClassInstances_;
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
    u16  mcpPort_ = 0;           // 0 = never asked for.
#if AVER_MODULE_SCENE && AVER_MODULE_SCRIPTING
    // Graph ABI state (load/attach/tick). One graph at a time.
    std::string  mcpGraphPath_;
    scene::Entity mcpGraphEntity_ = scene::kInvalidEntity;
    f32          mcpGraphTimeSeconds_ = 0.0f;
#endif

    void applyMcpCommand(const mcp::Command& c);

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
    bool mcpAbisRegistered_ = false;
#endif // AVER_MODULE_MCP
    bool releasedByUser_ = false;   // Shift+F1 during a session; cleared when the session ends

    // Play options (persisted in editor.ini). Declared unguarded for toolbar compatibility.
    enum class PlayMode : u8 {
        SelectedViewport = 0,   // play in the Level viewport, possessing the player (Alt+P)
        Simulate         = 1,   // the game runs, the player is not possessed, the editor keeps the camera (Alt+S)
        Standalone       = 2,   // a separate AverEngineRuntime.exe on the saved level (launchInRuntime)
        NewWindow        = 3,   // the viewport session, shown in a window of its own; Esc returns
    };
    enum class PlaySpawnAt : u8 { PlayerStart = 0, CameraLocation = 1 };
    PlayMode    playMode_ = PlayMode::SelectedViewport;    // Last launched; main button repeats it.
    PlaySpawnAt playSpawnAt_ = PlaySpawnAt::PlayerStart;
    // Default pawn walks (capsule: gravity, stairs, jumps, runs) without GameMode. Off = flying drone.
    bool        defaultPawnWalk_ = false;
    int32_t     walkCapsule_ = 0;
    int32_t     flyCapsule_ = 0;   // the flying default pawn's zero-gravity collider (0 = no-clip fallback)
    bool        playGameGetsMouse_ = true;   // Capture mouse when viewport session starts.
    std::string playStandaloneArgs_;         // Extra command line for Standalone Game.
    // Ejected (F8): editor has camera/input/tools; session keeps running. Possess (F8) to snap back.
    bool        playEjected_ = false;
    bool        playFrameStepPending_ = false;
    bool        scrollPrefsToPlay_ = false;  // "Advanced Settings..." scrolls Preferences to Play, once.
    // Start Play (refused while active, like button). Standalone goes through launchInRuntime.
    void launchPlay(Engine& e, PlayMode m);
    // Play From Here: viewport pawn starts at surface, facing camera yaw.
    void playFromHere(const Vec3& surface);
    std::optional<Vec3> playFromHere_;
    // Play in New Window: the device mirrors the game view into playWindow_ (IDevice::setMirrorWindow),
    // whose input feeds input_. The scene renders at the window's size; see docs/editor/PLAY_IN_NEW_WINDOW.md.
    std::unique_ptr<Window> playWindow_;
    rhi::IDevice* playWindowDevice_ = nullptr;
    u32 playWindowW_ = 0, playWindowH_ = 0;
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
    // Output log (thread-safe via logMutex_). Graph-print feed has timestamp.
    static constexpr size_t kMaxLogLines = 4000;
    std::mutex          logMutex_;
    std::deque<LogLine> logLines_;
    usize               logMultiRowCount_ = 0;   // Lines in logLines_ with rows > 1.
    struct GraphPrint { std::string text; f64 at; u32 count; };
    std::deque<GraphPrint> graphPrints_;
    bool                logAutoScroll_ = true;
    int                 logLevelFilter_ = 0;      // 0 = all, 1 = Info+, 2 = Warn+.
    // Console own scrollback (REPL transcript, not engine log).
    static constexpr size_t kMaxConsoleLines = 2000;
    std::deque<LogLine>      consoleLines_;
    bool                      consoleAutoScroll_ = true;
    bool                      consoleFocusPending_ = false;   // Armed by toggleDrawer(Console).
    char                      consoleInput_[256] = {};
    std::vector<std::string>  consoleHistory_;                // Most-recent-last.
    int                       consoleHistoryPos_ = -1;         // -1 = not recalling history.
    // Browse Variables tab filter box (separate from Content Browser's cbFilter_).
    char                      consoleBrowseFilter_[128] = {};
    // Content Browser folder (empty = content root) and Import source path.
    std::string         cbSelectedDir_;           // Empty = content root.
    char                importPath_[512] = {};
    bool                cbGallery_ = true;        // Tiles vs list.
    f32                 cbTileSize_ = 88.0f;      // Gallery tile edge (dp).
    std::string         cbSelectedFile_;          // Highlighted entry in file view.
    char                cbFilter_[128] = {};      // Search box (filters folder by name).
    // Search box scope: also look under open folder (cbGatherDeepMatches).
    bool                cbSearchDeep_ = false;
    std::string         cbEngineRoot_;            // Empty in shipped build.
    std::vector<std::string> cbHistory_;
    int                 cbHistoryPos_ = -1;
    // Right-click target (not necessarily selected).
    std::string         cbContextPath_;
    bool                cbContextIsDir_ = false;
    // Deferred popup requests.
    bool                cbWantRename_ = false, cbWantDelete_ = false, cbWantNewFolder_ = false;
    bool                cbWantDuplicate_ = false, cbWantImport_ = false;
    char                cbRenameBuf_[256] = {};
    char                cbNewFolderBuf_[128] = {};
    std::string         cbStatus_;                // Last operation outcome.
    int                 cbIdeChoice_ = -1;        // -1 = IdeIntegration default.
    // Bottom drawers: drawerAnim_ is eased 0..1 slide. drawerShown_ survives retraction.
    Drawer              drawer_ = Drawer::None;
    Drawer              drawerShown_ = Drawer::Content;
    f32                 drawerAnim_ = 0.0f;
    f32                 drawerFrac_ = 0.48f;      // Drawer height (fraction of work area).
    f32                 drawerRate_ = 14.0f;      // Slide easing rate.
    bool                cbDoubleClickEnter_ = true;   // Double-click folder to enter (vs single).
    bool                drawerRaise_ = false;     // Focus on frame it opens.
    // Drawer start path (--drawer content:<sub> or console:<sub>). Applied once at first draw.
    std::string         drawerStartSub_;
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
    bool                showRevisionControl_ = false;   // Window > Revision Control.
    std::string         rcProjectDir_;        // Project the latched answer describes.
    std::string         rcRoot_;              // Repository root, git spelling.
    // rcRoot_ normalized (lower-case, forward slashes, no trailing separator). Precomputed per latch.
    std::string         rcRootKey_;
    editor::RepoStatus  rcStatus_;
    std::string         rcWhy_;
    bool                rcAnswered_ = false;  // Query completed for rcProjectDir_ at least once.
    bool                rcGitPresent_ = false;
    f64                 rcRefreshedAt_ = -1.0;  // Last latch time (-1 = never).
    // Badge table (one entry per changed path, sorted for binary search).
    std::vector<std::pair<std::string, editor::FileStatus>> rcMarks_;
    // Row the panel is showing history/diff for.
    std::string         rcSelected_;
    std::vector<editor::LogEntry> rcLog_;
    std::vector<std::string>      rcDiff_;
    std::string         rcLogWhy_, rcDiffWhy_;
    bool                rcSelectedDiffable_ = false;   // Text asset that diff viewer can show.
    // Which diff side is on screen (file can have both: staged + edited again).
    editor::DiffSide    rcDiffSide_ = editor::DiffSide::Worktree;
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
    // Per-mesh LOD ladder (virtualized-geometry selection). Indexed by LEVEL.
    struct MeshLodLadder {
        std::vector<rhi::MeshHandle> handles;   // [level] -> MeshHandle.
        std::vector<u32> triCounts;              // [level] -> triangle count.
        std::vector<f32> errorCm;                // [level] -> world error.
        // [level] -> meshlets (bounds + cone, resident for telemetry).
        std::vector<std::vector<trifactor::ClusterView>> clusters;
    };
    std::unordered_map<u64, MeshLodLadder> meshLods_;
#endif

    // LOD knobs (persisted via RENDER.LODSELECT/LODTHRESHOLDPX in project settings).
    bool lodSelectEnabled_ = true;      // ON by default (--lod-select / editor toggle).
    f32  lodErrorThresholdPx_ = 1.0f;   // Projected screen error tolerance (--lod-error-px).
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    // LOD cluster stats (informational per-meshlet telemetry, CPU cost). OFF by default.
    bool lodClusterStatsEnabled_ = false;
    // This frame's selection counters (logged under "[LOD-SELECT]").
    struct LodSelectStats {
        u32 instancesTested = 0;
        u32 levelCollapsed = 0;        // Instances drew level > 0 this frame.
        u32 clustersTested = 0;        // Sum of chosen-level meshlet counts.
        u32 frustumCulled = 0;         // Informational cull count.
        u32 coneCulled = 0;            // Informational cull count.
        u64 trianglesBeforeLod0 = 0;   // Sum of LOD-0 triangle counts.
        u64 trianglesAfterLevel = 0;   // Sum of chosen level's triangle counts (what draws).
    };
    LodSelectStats lodStats_{};
    LodSelectStats lastLoggedLodStats_{};

    // Per-cluster LOD selection (virtualized geometry). Switchable via --lod-per-cluster.
    // Every meshlet across all LOD levels, mesh-local space, plus expanded global index list.
    struct MeshClusterData {
        std::vector<trifactor::MeshClusterView> clusters;   // Mesh-local; all levels.
        std::vector<std::vector<u32>> clusterIndices;        // [clusterId] -> expanded indices.
        std::vector<rhi::MeshVertex> verts;                  // Vertex copy (used by all clusters).

        // Instance-level shortcut precomputed inputs (built once at load).
        std::vector<trifactor::MeshClusterLevelBounds> levelBounds;
        f32 maxSphereRadius = 0.0f;   // Largest cluster sphere radius (mesh-local).
    };
    std::unordered_map<u64, MeshClusterData> meshClusterData_;

    // Per-instance cut-assembled MeshHandle (per instance, not per mesh). Rebuilt on cluster selection change.
    struct ClusterCutCache {
        std::vector<u32> selectedIds;   // Sorted; last frame's cut.
        rhi::MeshHandle handle = 0;     // 0 = none or selection was empty.
        u64 lastUsedFrame = 0;
        u64 rebuildCount = 0;           // How many times this instance's handle was rebuilt.
    };
    std::unordered_map<scene::Entity, ClusterCutCache> clusterCutCache_;
    u64 lodClusterFrame_ = 0;           // Incremented per onRender (drives sweep).
    bool lodPerClusterEnabled_ = false; // --lod-per-cluster. OFF = per-level path runs.

    // CPU-assembly upload cost measurement. Real std::chrono timing per createMesh call.
    struct LodClusterStats {
        u32 instancesTested = 0;
        u32 clustersTested = 0;
        u32 frustumCulled = 0;
        u32 coneCulled = 0;
        u32 lodRejected = 0;
        u32 clustersDrawn = 0;
        u64 trianglesBeforeLod0 = 0;
        u64 trianglesDrawn = 0;
        u32 instancesMixedLevels = 0;   // Instances with cut spanning > 1 level.
        u32 maxDistinctLevelsSeen = 0;  // Highest distinct-level count on any instance.
        u32 rebuilds = 0;               // Cache misses (cut changed) this frame.
        u32 cacheHits = 0;              // Cache hits (cut unchanged) this frame.
        u64 rebuildIndices = 0;         // Sum of assembled index counts on rebuilds.
        f64 rebuildMs = 0.0;            // Wall time in createMesh on rebuilds this frame.
        u32 instancesShortcut = 0;      // provablySingleLevelCut shortcuts this instance.
    };
    LodClusterStats lodClusterStats_{};
    LodClusterStats lastLoggedLodClusterStats_{};

    // GPU per-cluster LOD (--lod-mesh-shader). ASMain/MSClusterMain runs cut test per cluster.
    // Exists alongside CPU path (never replacing it). Mesh-shader-tier-0 devices use CPU path.

    // Per-mesh GPU cluster buffers (built once at load, only with --lod-mesh-shader).
    struct MeshClusterGpu {
        rhi::BufferHandle bounds = 0;   // ClusterBounds[] (t0).
        rhi::BufferHandle desc   = 0;   // ClusterMeshletDesc[] (t1).
        rhi::BufferHandle verts  = 0;   // Global vertex indices (t2).
        rhi::BufferHandle tris   = 0;   // Packed local triangles (t3).
        rhi::BindingSetHandle bindingSet = 0;
        u32 clusterCount = 0;           // All levels' clusters concatenated.
    };
    std::unordered_map<u64, MeshClusterGpu> meshClusterGpu_;
    bool lodMeshShaderEnabled_ = false;   // Resolved in onInit; see lodMeshShaderRequest_.
    // -1 = decide from DeviceCaps, 0 = --no-lod-mesh-shader, 1 = --lod-mesh-shader.
    int lodMeshShaderRequest_ = -1;

    // Pipeline creation: lazy, tried once. Tier-0 devices degrade to CPU path.
    bool lodMeshPipelineTried_ = false;
    bool lodMeshPipelineReady_ = false;
    // MaterialGraphRegistry revision at last compile (for latch reopening).
    u64  lodMeshPipelineGraphRev_ = ~0ull;
    rhi::ShaderHandle lodMeshAsShader_ = 0, lodMeshMsShader_ = 0, lodMeshPsShader_ = 0;
    rhi::PipelineHandle lodMeshPipeline_ = 0;
    rhi::PipelineLayout lodMeshLayout_{};   // srvCount=4 (the four cluster buffers).

    // Per-instance constant block for ASMain/MSClusterMain (ClusterFrameCB, b4 root CBV).
    struct ClusterFrameCB {
        f32 budgetPx = 0.0f;
        f32 projScale = 0.0f;
        f32 worldScale = 1.0f;
        u32 _pad = 0;
        f32 planes[6][4] = {};
    };

    // Informational counters for --lod-mesh-shader ("[LOD-MESH-SHADER]"). CPU-mirrored, not GPU readback.
    struct LodMeshShaderStats {
        u32 instancesTested = 0;
        u32 clustersDispatched = 0;      // Sum of MeshClusterGpu::clusterCount.
        u32 survivors = 0;               // Sum of survivor counts (CPU-mirrored).
        u64 trianglesDrawn = 0;
        u32 instancesMixedLevels = 0;
        u32 maxDistinctLevelsSeen = 0;
        u32 instancesShortcut = 0;       // Used provablySingleLevelCut shortcut.
    };
    LodMeshShaderStats lodMeshShaderStats_{};
    LodMeshShaderStats lastLoggedLodMeshShaderStats_{};
#endif

    // Depth proxy map (populated if Trifactor, empty if not). Each lookup answers 0 if no entry.
    std::unordered_map<rhi::MeshHandle, rhi::MeshHandle> depthProxy_;

    static rhi::MeshHandle depthProxyLookup(rhi::MeshHandle mesh, void* user);

    u32 sceneWalkReports_ = 0;     // Scene walk count (cost split reports at 2^n).

    Mat4 invVP_, viewProj_; Vec3 eye_{0,0,0};
};

} // namespace aver
