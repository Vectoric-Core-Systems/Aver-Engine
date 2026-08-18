// SandboxApp: the Aver editor executable. Viewport, gizmos, panels, Content Browser,
// and the frame loop that drives the runtime modules.

#include "aver/runtime/EntryPoint.hpp"
#include "aver/platform/Window.hpp"
#include "aver/platform/Splash.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/platform/Image.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Version.hpp"
#include "aver/formats/OcBeam.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/GltfImport.hpp"
#if AVER_MODULE_TRIFACTOR
#include "aver/trifactor/ClusterAdapt.hpp"
#endif
// The landscape runtime: a complete quadtree-LOD heightfield renderer that, before this change,
// nothing outside tests/landscape linked. See the member block near landscapeData_ below for how it
// is hosted -- independent of AVER_MODULE_SCENE, since a section is not an ECS entity.
#if AVER_MODULE_LANDSCAPE
#include "aver/formats/OcLand.hpp"
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
// tile coordinate + procedural-section synthesis (TerrainTile.hpp) that lets SandboxApp keep a small
// ring of streamed sections resident around the camera. See updateLandscapeRingTiles() below.
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
#endif
#include "aver/ui/ui_abi.h"

#include "ProjectBrowser.hpp"
#include "ProjectScaffold.hpp"
#include "ClusterMaterialShader.hpp"

// The material sampler register on the cluster pipeline. materialShaderDefines() is told the same
// number. s0, unconditionally: Voxi's own volume/shadow samplers (below) start at s1 instead of
// reusing Voxi's own s0/s1, so this slot never has to move depending on whether AVER_MODULE_VOXI
// is compiled in.
namespace { constexpr aver::u32 kClusterMaterialSamplerSlot = 0; }
#if AVER_MODULE_VOXI
// Stage 3: where the GPU per-cluster pipeline's table 0 puts Voxi's MERGED GI/shadow resources,
// relative to the 4 cluster-geometry SRVs (t0..t3) that are always there. See
// ensureLodMeshPipeline's own layout comment for the full register map, and
// modules/render.voxi/include/aver/voxi/VoxiGiShaders.hpp for what these three numbers feed.
namespace {
constexpr aver::u32 kClusterGiSrvBase       = 4;   // t4 the GI volume, t5 the shadow map
constexpr aver::u32 kClusterGiSamplerBase   = 1;   // s1 volume (linear-clamp), s2 shadow (comparison)
// b3, NOT kFeatureFrameConstantRegister (b4): the amplification/mesh shader half of this SAME
// pipeline already owns b4 for ClusterFrameCB (budget/frustum), and a root signature has exactly
// one cbuffer per register regardless of which stage declares it. b3 is unclaimed by anything else
// this layout declares -- see ensureLodMeshPipeline's constantDwords[3] assignment.
constexpr aver::u32 kClusterGiFrameRegister = 3;
}
#endif
#include "ToolsMenu.hpp"
#include "UiRegistry.hpp"

#if AVER_MODULE_MCP
#include "aver/mcp/McpBridge.hpp"
#endif
#include "ToolGlyphs.hpp"
#include "AssetEditor.hpp"
#include "ActorEditor.hpp"
#include "AnimEditor.hpp"
#include "GraphEditor.hpp"
#include "EditorEuler.hpp"
#include "EditorPrefs.hpp"
#include "EditorKeybinds.hpp"
#include "EditorEntitySnapshot.hpp"
#include "aver/platform/DirectoryWatcher.hpp"
#if AVER_HAVE_ROSLYN
#  include "aver/formats/AverDesign.hpp"
#endif
#include "EngineScaffold.hpp"
#include "McpConf.hpp"
#include "IdeIntegration.hpp"
#include "ShellIntegration.hpp"

#if AVER_MODULE_VOXI
#include "aver/voxi/Voxi.hpp"
#include "aver/voxi/VoxiRenderer.hpp"
// Stage 3: the narrow GI/shadow HLSL slice the GPU per-cluster pipeline composes into
// ClusterMaterialShader.hpp's PSClusterMain. See its own header comment for what this is and is not.
#include "aver/voxi/VoxiGiShaders.hpp"
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

#if AVER_MODULE_PBR
#include "aver/pbr/Material.hpp"
#include "aver/pbr/MaterialGpu.hpp"
#include "aver/pbr/MaterialSystem.hpp"
#include "aver/pbr/PbrShaders.hpp"
#include "aver/formats/OcMat.hpp"
#include "aver/formats/MaterialScript.hpp"
#include "aver/assets/TextureUpload.hpp"
#endif

#if AVER_MODULE_SCRIPTING
#include "aver/scripting/ScriptHost.hpp"
#endif

// AverSR (docs/AVERSR.md). Sandbox is the composition root that links Aver.Render.Sr -- see
// sandbox/CMakeLists.txt's own comment on the `if(TARGET Aver.Render.Sr)` block -- and constructs
// the concrete aver::sr::SpatialUpscaler; the generic RHI (aver/rhi/RHI.hpp, included above) never
// does and never will.
#if AVER_MODULE_SR
#include "aver/sr/AverSrQuality.hpp"
#include "aver/sr/AverSrSpatial.hpp"
#include "aver/sr/AverSrFxaa.hpp"
#endif

// physics_abi.h was nested inside AVER_MODULE_FRAMEWORK, but every use site below (aver_phys_init,
// aver_phys_step, aver_phys_ready, ...) is guarded on AVER_MODULE_PHYSICS alone. That was invisible
// as long as physics implied framework in practice, but AVER_MODULE_SCENE=OFF forces FRAMEWORK off
// (root CMakeLists) while leaving PHYSICS on by default, so this TU stopped seeing the header while
// its guarded call sites still expected it -- an unrelated-looking wave of "identifier not found".
#if AVER_MODULE_PHYSICS
#include "aver/physics/physics_abi.h"
#endif
#if AVER_MODULE_FRAMEWORK
#include "aver/framework/framework_abi.h"
#include "aver/framework/framework_hooks.h"
#endif

// windows.h was nested inside AVER_MODULE_SCENE, but the Win32 calls that actually need it --
// setMouseCaptured's ShowCursor/ClipCursor, applyMcpCommand's HWND, warpToAnchor's GetClientRect --
// are gated on _WIN32 (and, for the MCP one, AVER_MODULE_MCP) with no scene dependency at all. With
// SCENE off this TU lost the header while those guarded call sites still expected it.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#if AVER_MODULE_SCENE
#include "aver/scene/scene_abi.h"
#include "aver/anim/AnimSystem.hpp"
#include "aver/scene/World.hpp"
#include "aver/scene/Components.hpp"
// The placement -> entity loop, shared with the game runtime. See modules/world/README.md.
#include "aver/world/LevelInstance.hpp"
// Opt-in chunk streaming around the editor camera. See SandboxApp::setChunkStreamingEnabled.
#include "aver/world/ChunkWorld.hpp"
// A level's SCATTER records -> the generator's palette. The editor does not do this conversion
// itself: the game runtime needs the identical one, and one of the two would drift.
#include "aver/world/ScatterPalette.hpp"
#endif

#if AVER_WITH_IMGUI
#include "imgui.h"
#include "imgui_internal.h"
// The seam + the concrete Dear ImGui backend that plugs into it -- see UiBackend.hpp (Aver.RHI.D3D12)
// for the contract and ImGuiUiBackend.hpp (Aver.RHI.D3D12.ImGui, this file's other new link) for the
// factory. Needed here, not just inside the RHI, because installing a backend is the one thing only
// the app (not the RHI, which must not know ImGui exists) can decide to do.
#include "aver/rhi/d3d12/UiBackend.hpp"
#include "aver/rhi/d3d12/ImGuiUiBackend.hpp"
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
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
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aver {

// Gizmo axis basis and colours: X red, Y green, Z blue, amber highlight.
static const Vec3 kAxisDir[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
static const Vec3 kAxisCol[3] = {{0.92f, 0.24f, 0.24f}, {0.36f, 0.82f, 0.30f}, {0.30f, 0.55f, 1.0f}};
static const Vec3 kAxisHi = {1.0f, 0.80f, 0.15f};

// Appends a cube centred at (cx,cy,cz) with half-extent h.
static void appendBox(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx, f32 cx, f32 cy, f32 cz, f32 h) {
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
static void appendGround(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx, f32 s) {
    const u32 b = static_cast<u32>(v.size());
    v.push_back({-s,-s,0,0,0,1,-0.5f,-0.5f}); v.push_back({s,-s,0,0,0,1,0.5f,-0.5f});
    v.push_back({s,s,0,0,0,1,0.5f,0.5f}); v.push_back({-s,s,0,0,0,1,-0.5f,0.5f});
    idx.push_back(b); idx.push_back(b+1); idx.push_back(b+2); idx.push_back(b); idx.push_back(b+2); idx.push_back(b+3);
}
// Appends a UV sphere of radius r with +Z as the pole.
static void appendSphere(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx, f32 r, u32 rings, u32 sectors) {
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
// The Euler <-> quaternion pair now lives in EditorEuler.hpp so it can be tested. It was two static
// functions here, unreachable from any test, and eulerDegFromQuat's gimbal branch was wrong in a way
// that wrote corrupted rotations into saved levels.
using aver::editor::quatFromEulerDeg;
using aver::editor::eulerDegFromQuat;

// Rounds v to the nearest multiple of step; returns v unchanged when step is zero.
static f32 snapf(f32 v, f32 step) { return step > 0.0f ? std::round(v / step) * step : v; }

// Transforms a point by the row-vector matrix m.
static Vec3 xformPoint(const Mat4& m, const Vec3& p) {
    return { p.x*m.m[0][0]+p.y*m.m[1][0]+p.z*m.m[2][0]+m.m[3][0],
             p.x*m.m[0][1]+p.y*m.m[1][1]+p.z*m.m[2][1]+m.m[3][1],
             p.x*m.m[0][2]+p.y*m.m[1][2]+p.z*m.m[2][2]+m.m[3][2] };
}
// Transforms a direction by the row-vector matrix m, ignoring translation.
static Vec3 xformVec(const Mat4& m, const Vec3& v) {
    return { v.x*m.m[0][0]+v.y*m.m[1][0]+v.z*m.m[2][0],
             v.x*m.m[0][1]+v.y*m.m[1][1]+v.z*m.m[2][1],
             v.x*m.m[0][2]+v.y*m.m[1][2]+v.z*m.m[2][2] };
}
// Intersects a ray with an AABB. Returns true and writes the entry distance to tHit.
static bool rayAabb(const Vec3& o, const Vec3& d, const Vec3& mn, const Vec3& mx, f32& tHit) {
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
// Builds the floor grid line list out to extent ext with the given cell step.
static void buildGrid(std::vector<rhi::LineVertex>& v, f32 ext, f32 step) {
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
static void gzLine(std::vector<rhi::LineVertex>& v, const Vec3& a, const Vec3& b, const Vec3& c) {
    v.push_back({a.x,a.y,a.z, c.x,c.y,c.z}); v.push_back({b.x,b.y,b.z, c.x,c.y,c.z});
}
// Builds the unit-length translate arrow for axis a.
static std::vector<rhi::LineVertex> buildMoveAxis(int a, const Vec3& c) {
    std::vector<rhi::LineVertex> v;
    const Vec3 A = kAxisDir[a], P = kAxisDir[(a+1)%3], Q = kAxisDir[(a+2)%3];
    gzLine(v, {0,0,0}, A, c);
    const Vec3 tip = A, base = A * 0.80f;
    for (int k = 0; k < 4; ++k) { f32 t = k * (kPi * 0.5f); Vec3 r = P*(std::cos(t)*0.07f) + Q*(std::sin(t)*0.07f); gzLine(v, base+r, tip, c); }
    return v;
}
// Builds the unit rotation ring perpendicular to axis a.
static std::vector<rhi::LineVertex> buildRotRing(int a, const Vec3& c) {
    std::vector<rhi::LineVertex> v;
    const Vec3 P = kAxisDir[(a+1)%3], Q = kAxisDir[(a+2)%3];
    const int N = 64; Vec3 prev{};
    for (int k = 0; k <= N; ++k) { f32 t = k * (kTwoPi / N); Vec3 p = P*std::cos(t) + Q*std::sin(t); if (k > 0) gzLine(v, prev, p, c); prev = p; }
    return v;
}
// Builds the unit scale handle for axis a: a shaft with a box at the tip.
static std::vector<rhi::LineVertex> buildScaleAxis(int a, const Vec3& c) {
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

// The viewport manipulation mode. The four sculpt tools exist only where AVER_MODULE_LANDSCAPE does
// -- they are the same "which mouse-drag behaviour is active" idea Move/Rotate/Scale are, just aimed
// at a heightfield's samples instead of an object's transform, and they follow the same enum/toolbar
// shape rather than inventing a parallel one. See handleSculpt() and the ##vpbar_right toolbar block.
// WHICH MODE THE EDITOR IS IN: what the viewport is FOR right now.
//
// Select edits OBJECTS -- click to pick, gizmo to transform. Landscape edits TERRAIN -- click to
// sculpt, no picking, no gizmo. They are different activities with different meanings for the same
// mouse button, and that is the whole reason a mode exists rather than another tool.
//
// THIS USED TO BE ONE ENUM. Sculpt: Raise/Lower/Smooth/Flatten sat in `Tool` beside Move and Rotate,
// on keys 5-8, so terrain editing was a peer of "move an object": the gizmo still drew, picking
// still ran, the brush had nowhere of its own to put a radius or a strength, and nothing told you
// which of the two things a click was about to do. Splitting the mode out is what lets each side
// own its own toolbar, its own hotkeys 1..N, and its own answer to what clicking means.
enum class EditorMode { Select, Landscape };
static const char* kEditorModeNames[2] = {"Select", "Landscape"};

// Object tools. Only meaningful in EditorMode::Select.
enum class Tool { Select, Move, Rotate, Scale };
static const char* kToolNames[4] = {"Select", "Move", "Rotate", "Scale"};

#if AVER_MODULE_LANDSCAPE
// Terrain brushes. Only meaningful in EditorMode::Landscape.
enum class SculptTool { Raise, Lower, Smooth, Flatten };
static const char* kSculptToolNames[4] = {"Raise", "Lower", "Smooth", "Flatten"};
#endif

// Which bottom drawer is up. Only one at a time.
enum class Drawer { None, Content, Log };

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
// Applies the editor's dark ImGui colour scheme and metrics.
static void applyUnrealStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 3; s.FrameRounding = 3; s.GrabRounding = 3; s.TabRounding = 3;
    s.WindowBorderSize = 1; s.FrameBorderSize = 0; s.WindowPadding = ImVec2(8,8); s.FramePadding = ImVec2(7,4);
    s.ItemSpacing = ImVec2(7,5);
    ImVec4* c = s.Colors;
    const ImVec4 bg(0.086f,0.086f,0.094f,1), panel(0.129f,0.133f,0.145f,1), item(0.18f,0.185f,0.20f,1);
    const ImVec4 accent(0.95f,0.42f,0.13f,1), accentDim(0.95f,0.42f,0.13f,0.55f);
    c[ImGuiCol_WindowBg]=panel; c[ImGuiCol_ChildBg]=bg; c[ImGuiCol_PopupBg]=panel;
    c[ImGuiCol_Border]=ImVec4(0.03f,0.03f,0.03f,0.9f);
    c[ImGuiCol_FrameBg]=item; c[ImGuiCol_FrameBgHovered]=ImVec4(0.24f,0.25f,0.27f,1); c[ImGuiCol_FrameBgActive]=ImVec4(0.28f,0.29f,0.31f,1);
    c[ImGuiCol_TitleBg]=bg; c[ImGuiCol_TitleBgActive]=ImVec4(0.11f,0.11f,0.12f,1);
    c[ImGuiCol_MenuBarBg]=ImVec4(0.10f,0.10f,0.11f,1);
    c[ImGuiCol_Header]=item; c[ImGuiCol_HeaderHovered]=ImVec4(0.26f,0.27f,0.29f,1); c[ImGuiCol_HeaderActive]=accentDim;
    c[ImGuiCol_Button]=item; c[ImGuiCol_ButtonHovered]=ImVec4(0.26f,0.27f,0.29f,1); c[ImGuiCol_ButtonActive]=accentDim;
    c[ImGuiCol_CheckMark]=accent; c[ImGuiCol_SliderGrab]=accent; c[ImGuiCol_SliderGrabActive]=accent;
    c[ImGuiCol_Tab]=bg; c[ImGuiCol_TabHovered]=accentDim; c[ImGuiCol_Text]=ImVec4(0.86f,0.87f,0.88f,1);
    c[ImGuiCol_TextDisabled]=ImVec4(0.45f,0.46f,0.48f,1);
    c[ImGuiCol_Separator]=ImVec4(0.03f,0.03f,0.03f,1);
}
#endif

// One captured log line for the Output Log panel.
struct LogLine { LogLevel level; std::string text; };

// One entry in a Content Browser listing, with everything the views need already derived.
struct DirEntry {
    std::filesystem::path path;
    std::string full, name;
    bool isDir  = false;
    int  tile   = -1;      // sprite tile index, -1 for none
    bool module = false;
};

// A Content Browser directory listing, refreshed on a frame stamp. Folders sort first and are counted.
struct DirListing { int stamp = -1000; std::vector<DirEntry> entries; usize dirCount = 0; };

// The editor application: owns the scene, the panels, and the frame loop.
class SandboxApp final : public Application {
public:
    SandboxApp(u64 maxFrames, bool headless, std::string beamPath, std::string shot, Tool initialTool)
        : maxFrames_(maxFrames), headless_(headless), beamPath_(std::move(beamPath)), shot_(std::move(shot)), initialTool_(initialTool) {
        setLogSink(&SandboxApp::logSink, this);
    }

    // Appends one engine log line to the Output Log buffer. Must not itself log: the core log mutex is held.
    static void logSink(void* ctx, LogLevel level, std::string_view msg) {
        auto* self = static_cast<SandboxApp*>(ctx);
        std::lock_guard<std::mutex> lock(self->logMutex_);
        self->logLines_.push_back({level, std::string(msg)});
        if (self->logLines_.size() > kMaxLogLines) self->logLines_.pop_front();
    }

    // Returns the boot configuration for the editor window.
    BootConfig config() const override {
        BootConfig c; c.windowTitle="Aver Engine \xE2\x80\x94 Editor"; c.windowWidth=1600; c.windowHeight=900;
        c.maxFrames=maxFrames_; c.headless=headless_; c.useWarp=useWarp_;
        c.enableDebugLayer=debugLayer_;
        c.backend = backendName_.empty() ? nullptr : backendName_.c_str();
        return c;
    }
    void setUseWarp(bool w) { useWarp_ = w; }
    void setBackend(std::string b) { backendName_ = std::move(b); }   // --backend <name>
    void setDebugLayer(bool d) { debugLayer_ = d; }

#if AVER_WITH_IMGUI
    // Rebuilds the ImGui style and font atlas for the given DPI scale.
    void applyDpi(f32 dpi) {
        dpi_ = dpi;
        // Pushed rather than pulled: AssetEditor::draw() carries no dpi argument, and widening that
        // interface for one subclass is the wrong trade. ActorEditor's content-root setter set this
        // precedent; GraphEditor follows it.
        editor::setGraphEditorDpi(dpi_);
        applyUnrealStyle();
        if (dpi_ > 1.01f) ImGui::GetStyle().ScaleAllSizes(dpi_);

        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();
        fontMedium_ = nullptr;

        const std::string dir = executableDir();
        const std::string regular = dir + "\\Roboto-Regular.ttf";
        const std::string medium  = dir + "\\Roboto-Medium.ttf";
        const f32 px = 16.0f * dpi_;

        ImFont* body = fileExists(regular) ? io.Fonts->AddFontFromFileTTF(regular.c_str(), px) : nullptr;
        if (!body) {
            AVER_WARN("[Sandbox] '{}' missing or unreadable -- falling back to the built-in bitmap font", regular);
            io.Fonts->AddFontDefault();
            return;
        }
        io.FontDefault = body;
        if (fileExists(medium)) fontMedium_ = io.Fonts->AddFontFromFileTTF(medium.c_str(), px);
    }

    // Uploads branding/logo.png for the start screen. A missing file is a warning, not a failure.
    void loadLogo(Engine& e) {
        rhi::IResourceFactory* res = e.device()->resources();
        if (!res) return;

        const std::string path = executableDir() + "\\logo.png";
        ImageData img;
        std::string why;
        if (!decodeImage(path, img, &why)) {
            AVER_WARN("[Sandbox] '{}' not loaded ({}) -- the start screen falls back to a drawn badge",
                      path, why);
            return;
        }
        const int w = static_cast<int>(img.width), h = static_cast<int>(img.height);

        rhi::TextureDesc td;
        td.width = img.width;
        td.height = img.height;
        td.format = rhi::Format::RGBA8Unorm;
        td.bind = rhi::ResourceBind::ShaderResource;
        td.initialState = rhi::ResourceState::ShaderResource;
        td.debugName = "EditorLogo";
        const void* levels[1] = {img.pixels.data()};
        td.initialData = levels;
        td.initialDataCount = 1;
        td.initialRowPitch = img.rowPitch();
        logoTexture_ = res->createTexture(td);

        if (!logoTexture_) { AVER_WARN("[Sandbox] the start-screen mark could not be uploaded"); return; }
        logoUiId_ = e.device()->uiTextureId(logoTexture_);
        if (!logoUiId_) { AVER_WARN("[Sandbox] the start-screen mark is not reachable from the UI"); return; }
        logoAspect_ = h > 0 ? static_cast<f32>(w) / static_cast<f32>(h) : 1.0f;
        AVER_INFO("[Sandbox] start-screen mark decoded from {} ({}x{})", path, w, h);
    }

    // Uploads the Compile C# button's three-tile status sprite sheet. A miss is a warning.
    void loadCompileIcon(Engine& e) {
        rhi::IResourceFactory* res = e.device()->resources();
        if (!res) return;

        const std::string path = executableDir() + "\\compile-status.png";
        ImageData img;
        std::string why;
        if (!decodeImage(path, img, &why)) {
            AVER_WARN("[Sandbox] '{}' not loaded ({}) -- the Compile C# button falls back to a drawn dot",
                      path, why);
            return;
        }

        rhi::TextureDesc td;
        td.width = img.width;
        td.height = img.height;
        td.format = rhi::Format::RGBA8Unorm;
        td.bind = rhi::ResourceBind::ShaderResource;
        td.initialState = rhi::ResourceState::ShaderResource;
        td.debugName = "CompileStatusIcons";
        const void* levels[1] = {img.pixels.data()};
        td.initialData = levels;
        td.initialDataCount = 1;
        td.initialRowPitch = img.rowPitch();
        compileIconTexture_ = res->createTexture(td);
        if (!compileIconTexture_) { AVER_WARN("[Sandbox] the Compile C# icon could not be uploaded"); return; }
        compileIconUiId_ = e.device()->uiTextureId(compileIconTexture_);
        if (!compileIconUiId_) { AVER_WARN("[Sandbox] the Compile C# icon is not reachable from the UI"); return; }
        AVER_INFO("[Sandbox] Compile C# status icons decoded from {} ({}x{})", path, img.width, img.height);
    }

    // Uploads an N-tile sprite sheet staged next to the exe and measures its tile aspect. False on any miss.
    bool loadIconSheet(Engine& e, const char* file, int tiles, const char* debugName,
                       rhi::TextureHandle& outTex, u64& outId, f32& outAspect) {
        rhi::IResourceFactory* res = e.device()->resources();
        if (!res) return false;
        const std::string path = executableDir() + "\\" + file;
        ImageData img;
        std::string why;
        if (!decodeImage(path, img, &why)) {
            AVER_WARN("[Sandbox] '{}' not loaded ({}) -- the Content Browser falls back to drawn glyphs",
                      path, why);
            return false;
        }
        rhi::TextureDesc td;
        td.width = img.width; td.height = img.height;
        td.format = rhi::Format::RGBA8Unorm;
        td.bind = rhi::ResourceBind::ShaderResource;
        td.initialState = rhi::ResourceState::ShaderResource;
        td.debugName = debugName;
        const void* levels[1] = {img.pixels.data()};
        td.initialData = levels; td.initialDataCount = 1; td.initialRowPitch = img.rowPitch();
        outTex = res->createTexture(td);
        if (!outTex) { AVER_WARN("[Sandbox] '{}' could not be uploaded", path); return false; }
        outId = e.device()->uiTextureId(outTex);
        if (!outId) { AVER_WARN("[Sandbox] '{}' is not reachable from the UI", path); return false; }
        outAspect = (img.height > 0 && tiles > 0)
                  ? static_cast<f32>(img.width) / static_cast<f32>(tiles) / static_cast<f32>(img.height)
                  : 1.0f;
        AVER_INFO("[Sandbox] {} decoded from {} ({}x{}, {} tiles, aspect {:.3f})",
                  debugName, path, img.width, img.height, tiles, outAspect);
        return true;
    }
#endif

#if AVER_WITH_IMGUI
    // Plugs the editor's Dear ImGui backend into the device BEFORE Engine::run's own uiInit() call
    // runs against it -- see Application::onDeviceCreated's own comment for why this hook exists and
    // why onInit() (below) is too late. installUiBackend is a no-op if e.device() somehow is not a
    // D3D12 device (checked through IDevice::backend(), not assumed).
    void onDeviceCreated(Engine& e) override {
        uiBackend_.reset(rhi::d3d12::imgui_backend::create());
        rhi::d3d12::installUiBackend(e.device(), uiBackend_.get());
    }
#endif

    // Builds the editor: asset editors, MCP, physics, the placeholder scene, gizmos, and the render features.
    void onInit(Engine& e) override {
        AVER_INFO("[Sandbox] backend={} adapter='{}'", rhi::backendName(e.device()->backend()), e.device()->adapterName());
#if AVER_MODULE_SR
        // AverSR quality (docs/AVERSR.md "Quality levels"): sets the SAME renderScaleOverride_ knob
        // --render-scale drives, so an explicit --render-scale still wins over it -- same "still at
        // its 1.0 sentinel" precedent loadEditorPreferences() below already uses for --render-scale
        // vs. the saved pref. At Off (the default whether or not --aversr was even given) this line
        // does nothing, and neither does anything else guarded on averSrQuality_ below: Off stays
        // bit-identical to a tree with no AverSR in it, exactly as docs/AVERSR.md requires.
        if (averSrQuality_ != aver::sr::Quality::Off && renderScaleOverride_ == 1.0f)
            renderScaleOverride_ = aver::sr::renderScaleFor(averSrQuality_);
#endif
        // --render-scale F: applied once, here, before anything sizes itself off the device. 1.0 (no
        // flag) is a no-op -- setRenderScale clamps into [0.25,1] but a backend without a swapchain
        // yet just stores it for createSwapchainResources to pick up.
        if (renderScaleOverride_ != 1.0f) {
            e.device()->setRenderScale(renderScaleOverride_);
            AVER_INFO("[Sandbox] render scale {:.2f} (--render-scale)", e.device()->renderScale());
        }
#if AVER_MODULE_SR
        // Constructs SpatialUpscaler (a real GPU-resource-owning object, not the CLI-only
        // renderScaleOverride_ float above) so the editor genuinely holds a live aver::sr::IUpscaler
        // implementation whenever a non-Off level was requested on the command line. See
        // logAverSrActive()'s own comment for the honest limit of what that buys today.
        if (averSrQuality_ != aver::sr::Quality::Off) {
            ensureAverSrUpscaler(e.device());
            logAverSrActive(e.device());
        }
        // --edge-aa: see edgeAaEnabled_'s own member comment for why this shares AverSR's upscaler
        // slot and who wins when both are requested. Off (the default) never constructs FxaaResolve
        // and never touches the slot -- bit-identical to a build without this flag.
        if (edgeAaEnabled_) ensureEdgeAaUpscaler(e.device());
#endif
        // --depth-prepass: a same-frame depth-only pass ahead of the ordinary opaque colour walk --
        // see the entity loop's own comment (search "depth prepass phase") for the two-walk
        // mechanism this enables. Generic on IDevice (not gated on any module macro): it is a no-op
        // on any backend/feature that never implements depthPrepassPipeline(), exactly like
        // setUpscaler(non-null) is a no-op before any upscaler exists.
        if (depthPrepassOverride_) {
            e.device()->setDepthPrepassEnabled(true);
            AVER_INFO("[Sandbox] depth prepass enabled (--depth-prepass)");
        }

        // Registration order is precedence: the first factory that accepts a path wins.
        assetEditors_.registerFactory(&editor::makeMeshEditor);
        assetEditors_.registerFactory(&editor::makeActorEditor);
        assetEditors_.registerFactory(&editor::makeAnimEditor);
        // APPENDED, not inserted: AssetEditorHost::open() tries factories in registration order, so
        // moving this ahead of the others would change which editor claims a file they both accept.
        assetEditors_.registerFactory(&editor::makeGraphEditor);
        // Must run before any actor factory: the "is Roslyn available" answer is cached on first ask.
        locateAverDesign();
        {
            editor::ActorEditorHooks hooks;
            hooks.compileScripts = [this] { tools_.triggerToolbarCompile(project_); };
            hooks.compileBusy    = [this] { return tools_.compiling(); };
            hooks.drawCompileButton = [this] {
                tools_.drawCompileButton(project_, dpi_, compileIconUiId_);
            };
#if AVER_WITH_IMGUI
            hooks.openInIde      = [this](const std::string& p) {
                const editor::IdeInfo& ide = cbIde();
                if (!editor::openInIde(ide, p)) AVER_WARN("[Editor] could not open {} in {}", p, ide.name);
            };
            hooks.ideName = cbIde().name;
#endif  // AVER_WITH_IMGUI -- openInIde/ideName reach the content browser's IDE picker; without a
            // UI there is no picker and no actor editor to open a file from in the first place.
            editor::setActorEditorHooks(std::move(hooks));
        }
        window_ = e.window();

#if AVER_MODULE_MCP
        if (mcpPort_) {
            registerMcpAbis();
            // THE WIDGET HOOKS ARE UI-ONLY, and say so in the guard rather than by accident. uiReg_
            // is the ImGui widget registry; with AVER_ENABLE_UI=OFF it does not exist, and there
            // are no widgets for MCP to resolve or list either. Leaving these unregistered is the
            // honest answer -- MCP's other ABIs still work, so a UI-less editor keeps its control
            // channel and simply reports no widgets.
#if AVER_WITH_IMGUI
            mcp_.setWidgetResolver([this](const std::string& n, f32& x, f32& y) {
                return uiReg_.centreOf(n, x, y);
            });
            mcp_.setWidgetLister([this] { return uiReg_.describe(); });
#endif
            if (!mcp_.start(mcpPort_))
                AVER_WARN("[Mcp] --mcp was given but the channel did not start; the editor is "
                          "unaffected and carries on");
        }
#endif

        browser_.init();

#if AVER_MODULE_PHYSICS
        // Must start before any level loads: loading builds a static body per colliding placement.
        //
        // NO IMPLICIT GROUND. This used to add a 100 m box whose top face sat exactly on z = 0, so
        // Play-In-Editor had an invisible floor under every level whether it authored one or not --
        // a pit, a chasm or water below a level's own floor fell through to the same z = 0 plane a
        // level with nothing below it would. Matches the packaged runtime (GameApp::initPhysics):
        // a level supplies its own collision now.
        if (aver_phys_init()) {
            AVER_INFO("[Sandbox] physics started (fixed step {:.4f}s)", aver_phys_fixed_step());
        } else {
            AVER_WARN("[Sandbox] physics failed to start - gameplay will not collide");
        }
#endif

        // RESOLVED BEFORE THE PROJECT OPENS, AND THAT IS THE WHOLE POINT. This decision used to live
        // further down onInit, which reads as harmless -- it is the same code, a few hundred lines
        // later -- and silently disabled the entire GPU per-cluster path for the process lifetime.
        //
        // applyProject (below) does two things that consult lodMeshShaderEnabled_ while it was still
        // its default of false: loadProjectMeshes only builds meshClusterGpu_ under
        // `if (lodMeshShaderEnabled_)`, so no mesh ever got GPU cluster buffers; and
        // ensureLodMeshPipeline sets lodMeshPipelineTried_ BEFORE testing the flag, so that one early
        // call latched "already tried" forever. It is called from exactly one place, so nothing ever
        // asked again. The symptom was not a warning or a fallback message -- it was total silence:
        // no [LOD-MESH-SHADER] line of any kind in any log, while [LOD] cheerfully reported the path
        // "ON by default" a few hundred lines later, describing a flag that no longer reached
        // anything. Per-cluster frustum and cone culling have therefore never run in this editor.
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
        // OFF UNLESS ASKED FOR, and that is a DOWNGRADE from what this code said it did -- it
        // claimed "ON by default where the hardware allows". It was never on in any sense that
        // reached the GPU (see above), and the first run where it actually did showed why leaving it
        // that way would have been worse than the bug: the path was 22% faster and rendered the
        // scene WRONG. Frame 76.3ms -> 59.8ms, scene draw 51.7 -> 41.5, triangles 8M -> 2.9M, and
        // every plant in Electric Dreams drew as a black shredded silhouette.
        //
        // THE SHADING IS FIXED NOW, in two parts: PSClusterMain became a real material shader (see
        // ClusterMaterialShader.hpp), and the dispatch was taught to hand the material to the
        // CONTEXT rather than the device (see `ctx->setDrawBinding` beside dispatchMeshClusters).
        // Foliage on this path is textured, sun-lit and sky-ambient, and was compared side by side
        // against the ordinary path at the same camera to confirm it.
        //
        // STAGE 3 THEN GAVE IT SHADOWS AND GI TOO (still opt-in, still not the default): Voxi's
        // GI volume and shadow map are MERGED into this pipeline's own table 0 -- see
        // ensureLodMeshPipeline's register-map comment and D3D12Device.cpp's nullFill for why that
        // merge, rather than a third binding table, is what rhi::kBindingTableCount staying 2 buys
        // -- and PSClusterMain (ClusterMaterialShader.hpp) now runs the SAME shadowFactor()/
        // coneTracedIndirect() calls PSMainVoxi's own non-ray-traced fallback does, through
        // VoxiGiShaders.hpp's borrowed prelude.
        //
        // IT IS STILL NOT PARITY, which is why the default stays off. What is still missing, said
        // plainly rather than left to be discovered: NO RAY TRACING on this path, ever, even on a
        // project with ray tracing turned on -- shadows and reflections here are always the cascade
        // map and the voxel cone, Voxi's own fallback for when ray tracing is off. A project running
        // WITH ray tracing therefore gets a visibly different (softer, un-reflective) result on
        // cluster-path draws than on ordinary ones in the SAME frame. D3D12 ONLY, also unconditional:
        // see D3D12Device.cpp's nullFill for the pre-existing Vulkan defect this merge depends on
        // this backend not having. And a build without AVER_MODULE_VOXI gets none of this stage at
        // all -- see the two AVER_WARN branches below, which say which case is true out loud.
        lodMeshShaderEnabled_ = lodMeshShaderRequest_ > 0;
        if (lodMeshShaderEnabled_) {
            const rhi::DeviceCaps mcaps = e.device()->caps();
            // AND D3D12, WHICH THE COMMENT ABOVE ALREADY PROMISED AND THE CODE DID NOT KEEP.
            // Voxi's merge into this pipeline's table 0 puts a Texture3D, an acceleration structure
            // and structured buffers in a table that Vulkan's descriptorLayout()
            // (modules/rhi.vulkan/src/VulkanPipeline.cpp) still builds as if every table-0 slot were
            // a Texture2D -- a defect that predates this merge and is out of its scope to fix, but
            // one this path would walk straight into. meshShaderTier is NOT a proxy for the backend:
            // VulkanDevice sets it from VK_EXT_mesh_shader, so a Vulkan device with mesh shaders
            // passed every test here and selected the merged layout anyway.
            const bool ok = mcaps.meshShaderTier > 0 && mcaps.shaderModel >= 65 && mcaps.dxcAvailable
#if AVER_MODULE_VOXI
                            && e.device()->backend() == rhi::Backend::D3D12
#endif
                            ;
            lodMeshShaderEnabled_ = ok;
            if (ok) {
#if AVER_MODULE_VOXI
                // RECEIVES AND CASTS -- this used to say only the first half, and that gap is exactly
                // what the cluster-dispatch branch's own voxiRenderer_.submit() call now closes (search
                // "DEFECT 2's FIX" further down). IRenderFeature::submitDraw is still called from
                // exactly one place, D3D12Device::drawMesh, and this path still dispatches clusters
                // directly and still skips that call (see `if (!clusterDispatched)` further down) -- but
                // it no longer skips Voxi entirely: it calls VoxiRenderer::submit() itself, the same
                // public method submitDraw() only forwards to, with the SAME (mesh, world, material)
                // shape any ordinary drawMesh() instance already hands it. A cluster-drawn plant's
                // shadow proxy is now in the shadow cascade render and in the GI voxelisation exactly
                // like every other instance of its mesh; only its LIT geometry stays cluster-dispatched.
                // Never ray traced, still -- BLAS/RT-instance-table participation is a side effect of
                // this same fix (submit() is keyed on the FULL mesh handle) but PSClusterMain's own
                // shading remains fixed-function, per ClusterMaterialShader.hpp's own comment.
                AVER_INFO("[LOD] per-cluster mesh-shader path ON by request (mesh tier {}, SM {}) "
                          "-- faster and textured, and casts cascade shadows and voxel-cone GI through "
                          "its own depth proxy (via voxiRenderer_.submit()), though PSClusterMain's own "
                          "shading is never ray traced even when the project has ray tracing on",
                          mcaps.meshShaderTier, mcaps.shaderModel);
#else
                AVER_WARN("[LOD] per-cluster mesh-shader path ON by request (mesh tier {}, SM {}) "
                          "-- faster and textured, but NO SHADOWS AND NO GI on these draws (built "
                          "without AVER_MODULE_VOXI)",
                          mcaps.meshShaderTier, mcaps.shaderModel);
#endif
            }
            else    AVER_INFO("[LOD] per-cluster mesh-shader path unavailable (mesh tier {}, SM {}, "
                              "DXC {}); drawing without it", mcaps.meshShaderTier, mcaps.shaderModel,
                              mcaps.dxcAvailable);
        }
#endif

        if (!projectPath_.empty()) {
            // THROUGH THE SAME GATE AS A CLICK. A project named on the command line used to reach
            // browser_.open() directly, so a project from an older series opened with no prompt --
            // the upgrade question fired only for someone who double-clicked a card. Whether a
            // project is chosen by mouse or by argv is not a reason to migrate it silently.
            //
            // When the gate declines it has raised the modal, so the start screen is shown instead
            // of the editor and the author answers it there. That is why this arms the browser
            // rather than reporting a failure: nothing failed, a question is waiting.
            std::string err;
            if (browser_.openOrOfferUpgrade(projectPath_, &err)) applyProject(e);
            else if (browser_.upgradePending()) {
                armBrowser(true);
                AVER_INFO("[Sandbox] '{}' was made by an older series; asking before opening it",
                          projectPath_);
            } else {
                AVER_WARN("[Sandbox] '{}' not loaded: {}", projectPath_, err);
            }
        }
#if AVER_WITH_IMGUI
        if (e.device()->uiActive()) {
            applyDpi(e.window() ? e.window()->dpiScale() : 1.0f);
            AVER_INFO("[Sandbox] DPI scale {:.2f}, UI font rasterised at {:.0f}px", dpi_, 16.0f * dpi_);
            if (browserActive_) loadLogo(e);
            loadCompileIcon(e);
            loadIconSheet(e, "file-icons.png",   kFileIconTiles, "FileTypeIcons", fileIconsTexture_,   fileIconsUiId_,   fileIconAspect_);
            loadIconSheet(e, "folder-icons.png", kFolderIconTiles, "FolderIcons",  folderIconsTexture_, folderIconsUiId_, folderIconAspect_);
            loadIconSheet(e, "asset-icons.png",  kAssetIconTiles,  "AssetTypeIcons", assetIconsTexture_,  assetIconsUiId_,  assetIconAspect_);
            if (const std::string er = editor::engineRoot(); !er.empty()) {
                std::error_code ec;
                const std::filesystem::path cs = std::filesystem::path(er) / "scripting" / "csharp";
                if (std::filesystem::is_directory(cs, ec)) cbEngineRoot_ = cs.string();
            }
            AVER_INFO("[Sandbox] Content Browser engine root: {}",
                      cbEngineRoot_.empty() ? "(none - shipped build)" : cbEngineRoot_.c_str());
        }
#endif
        // Default blank map: ground floor + cube + sun + sky + atmosphere.
        std::vector<rhi::MeshVertex> gv, gi_v; std::vector<u32> gi, ci;
        appendGround(gv, gi, kEditorFloorHalf);
        rhi::MeshHandle ground = e.device()->createMesh(gv.data(), (u32)gv.size(), gi.data(), (u32)gi.size());
        // FROZEN: unitCube stays half-extent 1 -- .ocworld PLACEG scales are half-extents in cm applied to it.
        appendBox(gi_v, ci, 0,0,0, kEditorCubeHalf);
        rhi::MeshHandle cube = e.device()->createMesh(gi_v.data(), (u32)gi_v.size(), ci.data(), (u32)ci.size());
        std::vector<rhi::MeshVertex> uv_; std::vector<u32> ui_;
        appendBox(uv_, ui_, 0,0,0, 1.0f);
        rhi::MeshHandle unitCube = e.device()->createMesh(uv_.data(), (u32)uv_.size(), ui_.data(), (u32)ui_.size());

#if AVER_MODULE_SCENE
        // Built-in primitive meshes, keyed by fnv1a64 of the path a CMeshRenderer names.
        //
        // BOUNDS ARE RECORDED HERE TOO, and until 2026-08-02 they were not. Both primitives are
        // generated at radius/half-extent 1, so the box is exactly known -- but with meshBounds_
        // unset, every entity using one presented a DEGENERATE box to the frustum test at :1432,
        // which deliberately DRAWS a degenerate box rather than culling it (an entity whose bounds
        // were never filled in must not vanish). The consequence was that every primitive-using
        // entity was exempt from frustum culling entirely, including ones directly behind the
        // camera. Found while lifting this walk into Aver.Runtime.Game, where a five-placement
        // level reported "5 drawn, 0 culled" from every angle until these two lines existed.
        {
            const std::pair<Vec3, Vec3> unitBounds{Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}};
            std::vector<rhi::MeshVertex> sv; std::vector<u32> si;
            appendSphere(sv, si, 1.0f, 24, 48);
            const u64 sphereId = fnv1a64(std::string_view("Meshes/sphere.ocmesh"));
            const u64 cubeId   = fnv1a64(std::string_view("Meshes/cube.ocmesh"));
            sceneMeshes_[sphereId] = e.device()->createMesh(sv.data(), (u32)sv.size(), si.data(), (u32)si.size());
            sceneMeshes_[cubeId]   = unitCube;
            meshBounds_[sphereId]  = unitBounds;
            meshBounds_[cubeId]    = unitBounds;
            meshTris_[sphereId]    = static_cast<u32>(si.size() / 3);
            meshTris_[cubeId]      = static_cast<u32>(ci.size() / 3);
            // Kept so a dev check can build geometry of its own without re-uploading a cube.
            unitCubeMesh_ = unitCube;
            // Reported so the bounds above are OBSERVABLE rather than merely written. The editor
            // frames the camera on the whole level at load (frameCameraOn), so a headless run can
            // never show a primitive being culled -- everything is legitimately on screen. Without
            // this line the only way to tell the bounds exist is to read the source.
            AVER_INFO("[Mesh] {} built-in primitive(s), {} with bounds", sceneMeshes_.size(), meshBounds_.size());
        }

        // The named surfaces gameplay can ask for.
        {
            auto look = [this](const char* name, f32 r, f32 g, f32 b, f32 metal, f32 rough) {
                surfaceLooks_[aver_scene_material(0, name)] = SurfaceLook{{r, g, b}, metal, rough};
            };
            look("M_Floor",  0.22f, 0.23f, 0.26f, 0.02f, 0.85f);
            look("M_Wall",   0.48f, 0.50f, 0.55f, 0.03f, 0.72f);
            look("M_Trim",   0.30f, 0.33f, 0.38f, 0.35f, 0.45f);
            look("M_Crate",  0.62f, 0.44f, 0.22f, 0.02f, 0.78f);
            look("M_Target", 0.86f, 0.20f, 0.16f, 0.05f, 0.40f);
            look("M_Metal",  0.55f, 0.57f, 0.60f, 0.85f, 0.28f);
            look("M_Accent", 0.95f, 0.66f, 0.15f, 0.30f, 0.35f);
            // Three more generic surface names, kept for the same reason as the seven above: a
            // surface with neither a look nor an .ocmat renders the flat 0.80/0.80/0.85 fallback,
            // which is not wrong but reads as unfinished.
            //
            // THE COMMENT HERE USED TO CITE "the demo palette below" AND A "M_Foliage" DEFAULT.
            // Both are gone: buildDemoScatterPalette was deleted when levels learned to declare
            // their own SCATTER records, and OcScatterSpecies::material now defaults to empty
            // rather than naming a material one project happened to use. These three stay because
            // they are ordinary vocabulary any outdoor project might use -- not because a demo
            // needs them.
            look("M_Foliage", 0.16f, 0.42f, 0.14f, 0.0f, 0.85f);
            look("M_Bark",    0.35f, 0.24f, 0.15f, 0.0f, 0.85f);
            look("M_Rock",    0.42f, 0.40f, 0.37f, 0.05f, 0.80f);
        }
#endif

        MeshObj floor; floor.name="Floor"; floor.mesh=ground; floor.tris=(u32)gi.size()/3;
        floor.color[0]=0.34f; floor.color[1]=0.35f; floor.color[2]=0.37f;
        floor.metallic=0.0f; floor.roughness=0.9f;
        floor.aabbMin=Vec3{-kEditorFloorHalf,-kEditorFloorHalf,-5.0f};
        floor.aabbMax=Vec3{ kEditorFloorHalf, kEditorFloorHalf, 5.0f};
        objects_.push_back(floor);
        cubeMesh_ = cube; cubeTris_ = (u32)ci.size()/3;
        MeshObj c; c.name="Cube"; c.mesh=cube; c.tris=(u32)ci.size()/3; c.pos=Vec3{0,0,kEditorCubeHalf};
        c.color[0]=0.85f; c.color[1]=0.36f; c.color[2]=0.22f;
        c.metallic=0.1f; c.roughness=0.35f;
        objects_.push_back(c);

        if (!beamPath_.empty()) {
            fmt::OcBeamData beam; std::string err;
            if (fmt::loadOcbeam(beamPath_, beam, &err) && !beam.nodes.empty()) {
                std::vector<rhi::MeshVertex> bv; std::vector<u32> bi;
                AABB bb; bb.min=bb.max=Vec3{beam.nodes[0].x,beam.nodes[0].y,beam.nodes[0].z};
                for (auto& n : beam.nodes) bb.expand(Vec3{n.x,n.y,n.z});
                Vec3 ex=bb.extent(); f32 dot=(ex.x+ex.y+ex.z)*0.004f+0.5f;
                for (auto& n : beam.nodes) appendBox(bv,bi,n.x,n.y,n.z,dot);
                MeshObj cg; cg.name="Vehicle Cage"; cg.mesh=e.device()->createMesh(bv.data(),(u32)bv.size(),bi.data(),(u32)bi.size());
                cg.tris=(u32)bi.size()/3; cg.color[0]=0.85f;cg.color[1]=0.36f;cg.color[2]=0.22f;
                cg.aabbMin=bb.min; cg.aabbMax=bb.max;
                objects_.push_back(cg);
            }
        }

        for (MeshObj& o : objects_) makeMaterialFor(o);

        std::vector<rhi::LineVertex> gl; buildGrid(gl, kEditorGridHalf, kEditorGridCell);
        gridMesh_ = e.device()->createLineMesh(gl.data(), (u32)gl.size());

        // Per-mode gizmos: normal + amber-highlight variant of each axis.
        for (int a = 0; a < 3; ++a) {
            auto mv=buildMoveAxis(a,kAxisCol[a]);   gzMove_[a]  =e.device()->createLineMesh(mv.data(),(u32)mv.size());
            auto mh=buildMoveAxis(a,kAxisHi);       gzMoveHi_[a]=e.device()->createLineMesh(mh.data(),(u32)mh.size());
            auto rr=buildRotRing(a,kAxisCol[a]);    gzRot_[a]   =e.device()->createLineMesh(rr.data(),(u32)rr.size());
            auto rh=buildRotRing(a,kAxisHi);        gzRotHi_[a] =e.device()->createLineMesh(rh.data(),(u32)rh.size());
            auto sc=buildScaleAxis(a,kAxisCol[a]);  gzScale_[a] =e.device()->createLineMesh(sc.data(),(u32)sc.size());
            auto sh=buildScaleAxis(a,kAxisHi);      gzScaleHi_[a]=e.device()->createLineMesh(sh.data(),(u32)sh.size());
        }

#if AVER_MODULE_LANDSCAPE
        // The sculpt brush's footprint ring: buildRotRing(2, ...) is the ring PERPENDICULAR TO Z --
        // i.e. already flat IN the XY plane, which is the ground plane landscape heights are measured
        // from (see OcLand.hpp: "heights along +Z"). Reused rather than duplicated; only its radius
        // and world position (both per-frame, via the draw matrix) ever change.
        { auto bring = buildRotRing(2, kAxisHi); brushRing_ = e.device()->createLineMesh(bring.data(), (u32)bring.size()); }
#endif

#if AVER_MODULE_SCENE
        // The scene join. Registered HERE, before the Voxi renderer, for the reason spelled out
        // below: features run prePass in registration order and Voxi reads vertex buffers in its.
        skinnedScene_ = std::make_unique<aver::render::SkinnedScene>();
        if (skinnedScene_->init(*e.device())) {
            skinnedScene_->setResolvers(&SandboxApp::resolveAnimAsset, &SandboxApp::resolveSceneMesh, this);
            e.device()->addRenderFeature(skinnedScene_.get());
        } else {
            skinnedScene_.reset();   // init already said why; skinned entities draw at rest
        }

        // --skin-scene-test <dir>: the same question as --skin-draw-test but through the WHOLE
        // chain -- a real .ocmesh with skin streams, a real .ocskel, a real .ocanim, AnimSystem,
        // SkinnedScene's per-entity target, and the substituted draw handle.
        if (!skinSceneDir_.empty() && skinnedScene_) {
            skinScene_ = std::make_unique<aver::editor::SkinSceneTest>();
            u64 meshId = 0, skelId = 0, clipId = 0;
            u32 meshHandle = 0;
            if (skinScene_->setup(e, skinSceneDir_, &meshId, &skelId, &clipId, &meshHandle)) {
                // The draw pass looks the entity's mesh up in sceneMeshes_, and SkinnedScene
                // resolves through the SAME table. Registering here is what makes a spawned
                // entity actually reach a draw call without a project having been opened.
                sceneMeshes_[meshId] = meshHandle;
                skinScene_->restBounds(meshBounds_[meshId].first, meshBounds_[meshId].second);
                // The three ids the entities name, pointed at the cooked files. contentIndex_ is
                // what AnimSystem and SkinnedScene both resolve through, so registering here is
                // what makes the rig reachable without a project.
                std::string d = skinSceneDir_;
                while (!d.empty() && (d.back() == 92 || d.back() == '/')) d.pop_back();
                contentIndex_[meshId] = d + "/Rig.ocmesh";
                contentIndex_[skelId] = d + "/Rig.ocskel";
                contentIndex_[clipId] = d + "/Rig_Bend.ocanim";
                anim::animSystem().setResolver(&SandboxApp::resolveAnimAsset, this);
                objects_.clear();   // nothing unrelated on screen; the probes classify by colour
                sel_ = -1;
            } else {
                skinScene_.reset();
            }
        } else if (!skinSceneDir_.empty()) {
            AVER_ERROR("[Skin] --skin-scene-test needs the scene join, which did not initialise");
        }
#endif

        // --furnace-test: does the shading model CONSERVE ENERGY? A uniform environment of
        // radiance L and surfaces of albedo 1 -- every one must read the same, whatever its
        // orientation and whatever surrounds it.
        if (furnaceTest_) {
            objects_.clear();
            sel_ = -1;
            sky_.furnaceRadiance = 0.25f;
            sky_.furnaceSun = furnaceSun_;
            sunAmbient_ = 1.0f;

            // Albedo ONE, fully rough, non-metallic: the furnace's premise is a perfect Lambertian
            // white, and any of those three wrong makes the answer legitimately not L.
            const auto white = [&](const char* nm, Vec3 pos, Vec3 scale) {
                MeshObj o;
                o.name = nm;
                o.mesh = unitCubeMesh_;
                o.pos = pos; o.scale = scale;
                o.color[0] = o.color[1] = o.color[2] = 1.0f;
                o.metallic = 0.0f; o.roughness = 1.0f;
                o.aabbMin = Vec3{-scale.x*1.1f, -scale.y*1.1f, -scale.z*1.1f};
                o.aabbMax = Vec3{ scale.x*1.1f,  scale.y*1.1f,  scale.z*1.1f};
                objects_.push_back(o);
            };
            // A flat slab, and a tall one. Different faces point in different directions, so the
            // probes across them sample several orientations of the same white surface.
            white("FurnaceFloor", Vec3{0, 0, -30}, Vec3{900.0f, 900.0f, 20.0f});
            white("FurnaceTall",  Vec3{0, 0, 200}, Vec3{160.0f, 160.0f, 220.0f});
            // THE ONE THAT MATTERS: a box open only toward the camera, so the surface at its back
            // is occluded from most of the hemisphere. In a furnace it must STILL read L, because
            // the walls occluding it are emitting L too. This is where an occlusion term that
            // forgets the occluder is also a light source shows up.
            white("FurnaceCaveBack",  Vec3{-520, -520, 160}, Vec3{20.0f, 200.0f, 200.0f});
            white("FurnaceCaveLeft",  Vec3{-330, -700, 160}, Vec3{200.0f, 20.0f, 200.0f});
            white("FurnaceCaveTop",   Vec3{-330, -520, 350}, Vec3{200.0f, 200.0f, 20.0f});
        }

        // --refl-test: are ray-traced reflections GLOBAL? A mirror, and a beacon parked on its
        // reflection vector far outside the voxel volume, run past both tracers.
        if (reflTest_) {
            refl_ = std::make_unique<aver::editor::ReflTest>();
            objects_.clear();
            sel_ = -1;

            // The mirror: a large flat quad at z=0, fully metallic and almost perfectly smooth, so
            // what it shows is almost entirely the reflection rather than its own colour.
            MeshObj m;
            m.name = "ReflMirror";
            m.mesh = unitCubeMesh_;
            m.pos = Vec3{0, 0, -20.0f};
            // HALF-EXTENTS IN CENTIMETRES: the unit cube is half-extent 1, so this is an 1800 cm
            // mirror. The first version used 18 and made a 36 cm slab that every probe missed.
            m.scale = Vec3{1800.0f, 1800.0f, 20.0f};
            m.color[0] = 0.02f; m.color[1] = 0.02f; m.color[2] = 0.02f;
            m.metallic = 1.0f; m.roughness = 0.03f;
            m.aabbMin = Vec3{-1900, -1900, -60}; m.aabbMax = Vec3{1900, 1900, 20};
            objects_.push_back(m);

            const Vec3 bp = aver::editor::ReflTest::beaconPosition(camPos_, Vec3{0, 0, 0});
            MeshObj bcn;
            bcn.name = "ReflBeacon";
            bcn.mesh = unitCubeMesh_;
            bcn.pos = bp;
            // Big enough to subtend several degrees from 5200 cm away, or the reflection of it
            // lands between probes.
            bcn.scale = Vec3{700.0f, 700.0f, 700.0f};
            // Emissive-bright green: nothing else in this scene is green, so a probe that turns
            // green can only be showing the beacon.
            bcn.color[0] = 0.02f; bcn.color[1] = 0.95f; bcn.color[2] = 0.05f;
            bcn.roughness = 1.0f;
            bcn.aabbMin = Vec3{-950, -950, -950}; bcn.aabbMax = Vec3{950, 950, 950};
            objects_.push_back(bcn);
            reflBeaconIndex_ = static_cast<int>(objects_.size()) - 1;

            refl_->setBeaconPosition(bp);
            refl_->setVolumeExtent(giExtent_);
        }

        // --skin-draw-test: the same question one level up -- does anything DRAW the skinned buffer.
        //
        // THIS BLOCK'S POSITION IS THE CONTRACT. addRenderFeature is a plain push_back and prePass
        // runs features in registration order, so a skinning dispatch must be registered BEFORE the
        // scene renderer -- Voxi replays the shadow and voxelise passes in ITS prePass, and those
        // read the vertex buffer. Registered after, they read it every frame before it is written:
        // a pose-behind shadow, and a barrier claiming Common on a resource the runtime has already
        // seen bound as a vertex buffer. The debug layer is what said so, once per frame.
        if (skinDrawTest_) {
            skinDraw_ = std::make_unique<aver::editor::SkinDrawTest>();
            if (skinDraw_->init(*e.device())) {
                e.device()->addRenderFeature(skinDraw_.get());
                // The scene becomes exactly one object, so the probe reads the box or the sky and
                // never an unrelated mesh that happens to sit behind it.
                objects_.clear();
                sel_ = -1;
                // The ground first, so it is behind the box in submission order as well as in
                // depth. Pale and rough, because the assertion is about how much light reaches it.
                MeshObj g;
                g.name = "SkinDrawTest.Ground";
                g.mesh = skinDraw_->ground();
                g.color[0] = 0.75f; g.color[1] = 0.75f; g.color[2] = 0.72f;
                g.roughness = 0.9f;
                g.aabbMin = Vec3{-1700, -1700, -300};
                g.aabbMax = Vec3{ 1700,  1700, -160};
                objects_.push_back(g);

                MeshObj o;
                o.name = "SkinDrawTest";
                o.mesh = skinDraw_->mesh();
                o.color[0] = 0.9f; o.color[1] = 0.15f; o.color[2] = 0.9f;   // magenta: unlike ground and sky in every channel
                o.roughness = 0.6f;
                o.aabbMin = Vec3{-260, -260, -260};
                o.aabbMax = Vec3{ 260,  260,  260};
                objects_.push_back(o);

                // The ground probe is only an acceleration-structure test when ray tracing is what
                // draws the shadow. Told rather than guessed, so the report can say which of the
                // two questions it actually answered.
                // The structure half needs a device that can ray-trace. Whether it is ON is the
                // test's own schedule to drive, not a launch flag: the experiment IS the toggle.
                skinDraw_->setRayTracingAvailable(e.device()->caps().rayTracingTier >= 11);
            } else {
                AVER_ERROR("[Skin] draw test unavailable on this device");
                skinDraw_.reset();
            }
        }
#if AVER_MODULE_SCRIPTING
        {
            scripting::HostDesc hd;
            hd.bridgeDir = executableDir() + "\\Scripting";
            hd.scriptsDir = resolveScriptsDir();
            scripts_.init(hd);

            // GRAPH-AS-CLASS CATCH-UP, for a project opened from the COMMAND LINE. applyProject's own
            // "Starting scripts" stage already tries scripts_.declareGraphClasses/spawnClassPlacements
            // (see that function's own comment), but a project named on argv is opened at ~line 850,
            // ABOVE this block -- BEFORE the scripting host bootstraps at all, so scripts_.ready() was
            // false and that attempt was a documented no-op. This is the SAME pre-existing ordering
            // gap applyProject's own `if (scripts_.ready() && scriptsDir_.empty() ...)` guard already
            // has for a project's own compiled C# scripts (unaffected by this change, and out of this
            // slice's scope to fix generally) -- but graph classes are new with this slice and must not
            // silently inherit a defect neither modules/runtime.game/src/GameApp.cpp (which has no such
            // two-phase boot order) nor a project opened through the in-editor browser (already past
            // this point in onInit by the time a human can click anything) ever exhibits. Both calls
            // are idempotent (aver_fw_class_declare by name; spawnClassPlacements only touches
            // classPlacements_, cleared by unloadLevel/loadLevel), so calling them again here even when
            // applyProject's own attempt already succeeded changes nothing.
#if AVER_MODULE_FRAMEWORK
            if (scripts_.ready() && project_.valid()) {
                const i32 graphClasses = scripts_.declareGraphClasses(project_.contentDir());
                if (graphClasses > 0)
                    AVER_INFO("[Graph] {} graph class(es) declared from '{}'", graphClasses, project_.contentDir());
                spawnClassPlacements();
            }
#endif

#if AVER_MODULE_VOXI
        // Hand the GPU's real capabilities to Voxi so its settings reflect this hardware.
        {
            // `caps`, not `c`: a MeshObj named `c` is still in scope from the editor-cube setup
            // two hundred lines up, and shadowing it warned (C4456).
            const rhi::DeviceCaps caps = e.device()->caps();

            // VIRTUALIZED GEOMETRY IS ON BY DEFAULT WHERE THE HARDWARE ALLOWS IT, and it used to be
            // opt-in behind --lod-mesh-shader. That default was indefensible: this project's
            // pine_tree_01 is 17.18 MILLION triangles, fifteen of them are hand-placed, and with the
            // flag off the editor drew every one at LOD0 -- around 258 million triangles for a scene
            // whose status bar reads "2 actors", at 13 FPS. The feature built precisely to make that
            // tractable sat behind a switch nobody opening the editor would know to throw.
            //
            // AUTO, NOT FORCED: -1 means "decide from caps", and --lod-mesh-shader / --no-lod-mesh-
            // shader pin it either way. The gate is the same one createShader enforces, so a device
            // that would refuse the pipeline never gets asked for it and silently keeps the CPU path.
            //
            // THE DECISION ITSELF HAS MOVED, to just before the project opens -- see the comment
            // there. It has to happen before applyProject, because applyProject is what loads the
            // meshes whose GPU cluster buffers are built only when this flag is already true.
            // Deciding it here, after that, is exactly the bug that kept the path dead.

            voxi::DeviceInfo di;
            di.msaaMask = caps.msaaMask; di.maxMsaaSamples = caps.maxMsaaSamples;
            di.rayTracingTier = caps.rayTracingTier; di.computeShaders = caps.computeShaders;
            di.typedUavLoads = caps.typedUavLoads; di.conservativeRaster = caps.conservativeRaster;
            di.shaderModel = caps.shaderModel; di.meshShaderTier = caps.meshShaderTier;
            di.dxcAvailable = caps.dxcAvailable;
            voxi::Renderer::get().setDeviceInfo(di);
            voxi::Settings s = voxi::Renderer::get().settings();
            s.msaa = static_cast<voxi::Msaa>(e.device()->sampleCount());
            if (msaaOverride_) s.msaa = static_cast<voxi::Msaa>(msaaOverride_);
            // --no-gi wins over --gi.
            if (giForceOff_)     s.globalIllumination = voxi::Quality::Off;
            else if (giOverride_) s.globalIllumination = static_cast<voxi::Quality>(giOverride_);
            // --no-rt wins over --rt, mirroring --no-gi above. IT HAS TO EXIST AS ITS OWN FLAG because
            // rtOverride_ uses 0 for "not given" and Quality::Off is also 0, so there is no value of
            // --rt that means off. Ray tracing now defaults to Medium, which made the Off rung of the
            // published cost ladder unreachable from the command line -- the row was measurable only
            // back when Off was the default and no flag was needed to reach it.
            if (rtForceOff_)     s.rayTracing = voxi::Quality::Off;
            else if (rtOverride_) s.rayTracing = static_cast<voxi::Quality>(rtOverride_);
            if (msOverride_) s.meshShaders = true;
            // Applied to `s` (not voxiRenderer_ directly) and BEFORE it reaches the singleton below:
            // voxiRenderer_.setSettings() is called again every frame with whatever
            // voxi::Renderer::get().settings() holds, so an override poked into voxiRenderer_
            // afterward, once, would be silently overwritten back to the default on the very next
            // frame. A NEGATIVE COUNT IS REPORTED rather than cast to a huge unsigned one and
            // clamped: the warning that came out of that read "4294967291 shadow rays clamped to
            // 32", which describes the cast and not the typo that caused it.
            if (rtRaysOverride_ > 0) s.rtShadowRays = static_cast<u32>(rtRaysOverride_);
            else if (rtRaysOverride_ < 0)
                AVER_WARN("[Sandbox] --rt-rays {} is not a ray count; the default of {} stands",
                          rtRaysOverride_, s.rtShadowRays);
            // >= 0, not > 0: 0 is a MEANING here (filter off), not "flag absent" -- the sentinel
            // is -1, unlike its two neighbours whose valid range starts at 1.
            if (rtShadowDenoiseOverride_ >= 0) s.rtShadowDenoise = static_cast<u32>(rtShadowDenoiseOverride_);
            if (rtPixelsPerRayOverride_ > 0) s.rtPixelsPerRayTile = static_cast<u32>(rtPixelsPerRayOverride_);
            else if (rtPixelsPerRayOverride_ < 0)
                AVER_WARN("[Sandbox] --rt-pixels-per-ray {} is not a tile edge; the default of {} stands",
                          rtPixelsPerRayOverride_, s.rtPixelsPerRayTile);
            if (giUpdateIntervalOverride_ > 0) s.giUpdateInterval = static_cast<u32>(giUpdateIntervalOverride_);
            else if (giUpdateIntervalOverride_ < 0)
                AVER_WARN("[Sandbox] --gi-update-interval {} is not a frame count; the default of {} stands",
                          giUpdateIntervalOverride_, s.giUpdateInterval);
            voxi::Renderer::get().setSettings(s);
            AVER_INFO("[Voxi] attached: MSAA {}x, RT tier {}, SM {}, mesh tier {}", caps.maxMsaaSamples, caps.rayTracingTier, caps.shaderModel, caps.meshShaderTier);

            // Registration is non-owning: voxiRenderer_ must outlive the device, torn down in onShutdown.
            // Read back from the singleton rather than reusing `s` directly, so this sees the same
            // clamping voxi::Renderer::setSettings just applied.
            voxiRenderer_.setSettings(voxi::Renderer::get().settings());
            if (frameTimeReport_) voxiRenderer_.setFrameTimeReport(true);
            if (voxiRenderer_.init(*e.device())) {
                e.device()->addRenderFeature(&voxiRenderer_);
                voxiAttached_ = true;
                if (projectRenderPending_) applyProjectRenderSettings();
                if (saveProject_ && !saveProjectDone_) { saveProjectDone_ = true; seedAndSaveProject(); }
#if AVER_WITH_IMGUI
                // --import's deferred handshake, UI-only on purpose: importAsset belongs to the
                // content-browser half of this file and calls cbIsEditable/cbInvalidate/importModel,
                // which are the browser's own. A UI-less editor has no browser to import into.
                if (!importSrc_.empty() && !importDone_) {
                    importDone_ = true;
                    importAsset(importSrc_, importDst_);
                }
#endif
#if AVER_MODULE_PBR
                textureFactory_ = e.device()->resources();
                voxiRenderer_.materials().setTextureResolver(&SandboxApp::resolveMaterialTexture, this);
#endif
                // Installed unconditionally, even in a build with no Trifactor: depthProxy_ is then
                // simply empty, every lookup answers 0, and every pass draws what it drew before.
                voxiRenderer_.setDepthProxy(&SandboxApp::depthProxyLookup, this);
            }
        }
#endif
        // The game UI's render feature: overlay pass only, so ordering against scene features is free.
        gameUi_ = aver::render::ui::UiRenderer::create(*e.device());
        if (gameUi_) e.device()->addRenderFeature(gameUi_);

#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
        // NOT an IRenderFeature: it has no prePass/scenePass of its own, only two methods
        // renderSceneEntities() calls directly from inside the CPU entity walk -- see occluder_'s own
        // member comment. Built unconditionally (whether or not --occlusion-cull was given): it is
        // cheap to own and every call it drives is already gated on occlusionCullEnabled_.
        if (rhi::IResourceFactory* occRes = e.device()->resources()) {
            occluder_ = aver::occlusion::createOcclusionCuller(*occRes);
            // WARMED UP HERE, NOT LEFT TO ITS FIRST PER-FRAME CALL, and that is not an optimisation --
            // it is what keeps this safe. ensureSized() is where the module's THREE compute pipelines
            // actually get built (createComputePipeline, D3D12ResourceFactory::pipelines_.push_back),
            // and every OTHER feature in this engine builds every one of its own PSOs during init()
            // too, before any frame is being recorded -- see VoxiRenderer::init calling
            // createScenePipelines etc. That is not a style preference: D3D12RenderContext caches the
            // CURRENTLY BOUND pipeline as a raw pointer into that same vector (pipe_, read by
            // setBindingSet's register-mismatch check on every draw) between one setPipeline() call
            // and the next, and pipelines_.push_back() reallocating while some OTHER feature's pointer
            // is still resting on the OLD backing array is a dangling-pointer read -- reached in
            // practice: calling ensureSized() lazily from inside the scene walk (this module's first
            // attempt) corrupted Voxi's own cached pipeline pointer mid-frame and crashed the driver's
            // shader compiler on the very next PSO the corrupted state touched. Warming up here, before
            // Engine::run's loop ever calls beginFrame() for the first time, means the one-time
            // allocation that can invalidate pipe_ happens while nothing has a live pointer into
            // pipelines_ to begin with. A later resize (window resize, --render-scale change) still
            // rebuilds the pyramid lazily from inside the frame -- accepted for now: it recreates the
            // SEED pipeline only, not new SLOTS in pipelines_ every frame in steady state (see
            // OcclusionCuller.cpp's own comment on why only sample-count changes touch that one), and
            // a resize is already a rare, user-driven event this task's --frames runs never exercise.
            rhi::TextureDesc occSceneDesc;
            if (const rhi::TextureHandle occDepth = e.device()->sceneDepthTexture();
                occDepth && occRes->textureInfo(occDepth, occSceneDesc)) {
                occluder_->ensureSized(*occRes, occSceneDesc.width, occSceneDesc.height, e.device()->sampleCount());
            }
        }
#endif

#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
        // Registered unconditionally (like skinnedScene_ above), whether or not --particle-test was
        // given: a project with its own CParticleEmitter placements must draw them without needing a
        // command-line flag, exactly the reasoning voxiRenderer_'s own registration comment gives.
        // The process-global system resolves effect ids through the process-global library --
        // both singletons, matching scene::World::instance()/anim::animSystem() -- so a
        // registerEffect() anywhere in this process reaches whatever ticks CParticleEmitter,
        // whether or not particleRenderer_ itself came up (a device with no GPU resource factory
        // still simulates; it just has nothing to draw with).
        particles::particleSystem().setEffectLibrary(&particles::particleEffects());
        if (particleRenderer_.init(*e.device())) {
            particleRenderer_.setSystem(&particles::particleSystem());
            e.device()->addRenderFeature(&particleRenderer_);
            particlesAttached_ = true;
#if AVER_MODULE_VOXI
            // DECIDED 4's seam, installed only once Voxi has actually attached (voxiAttached_) -- see
            // particleGiPrepare/particleGiBind's own comment for the whole contract. particleRenderer_
            // never learns Voxi's name; this is the one call site that hands it a way to reach it.
            // --no-particle-gi is the A/B toggle the PROOF asks for: the SAME scene, the SAME Voxi
            // volume, with only this one line skipped, so a screenshot difference is attributable to
            // nothing but this seam.
            if (voxiAttached_ && !noParticleGi_) {
                particles::ParticleRenderer::GiSeam seam;
                seam.prepare = &SandboxApp::particleGiPrepare;
                seam.bind = &SandboxApp::particleGiBind;
                seam.user = this;
                particleRenderer_.setGiSeam(seam);
            }
#endif
        } else {
            AVER_ERROR("[Particles] renderer unavailable on this device");
        }

        // --particle-test: a dust cloud (DECIDED: not a weapon effect) straddling an opaque cube, so
        // the transparent pass's own depth test (DECIDED 1) is visible in one screenshot -- some
        // particles nearer the camera than the cube, some farther and hidden behind it. Author's own
        // test content, nowhere near either demo project.
        if (particleTest_) {
            objects_.clear();
            sel_ = -1;

            MeshObj occluderCube;
            occluderCube.name = "ParticleTest.Occluder";
            occluderCube.mesh = unitCubeMesh_;
            occluderCube.pos = Vec3{600.0f, 0.0f, 50.0f};
            occluderCube.scale = Vec3{80.0f, 80.0f, 80.0f};
            occluderCube.color[0] = 0.55f; occluderCube.color[1] = 0.5f; occluderCube.color[2] = 0.45f;
            occluderCube.roughness = 0.8f;
            occluderCube.aabbMin = Vec3{512.0f, -88.0f, -38.0f};
            occluderCube.aabbMax = Vec3{688.0f,  88.0f, 138.0f};
            objects_.push_back(occluderCube);

            // A slow-drifting dust cloud, EmitterShape::Box spanning well in front of AND behind the
            // cube above along the camera's forward axis (+X) -- see this block's own comment for why
            // that spread is what makes the occlusion visible in a single static frame rather than
            // needing the camera or the particles to move.
            particles::ParticleEffect fx;
            fx.shape = particles::EmitterShape::Box;
            fx.shapeSize = Vec3{350.0f, 60.0f, 90.0f};
            fx.emissionRate = 150.0f;
            fx.burstCount = 0;
            fx.maxParticles = 400;
            fx.lifetimeMin = 3.0f;
            fx.lifetimeMax = 5.0f;
            fx.direction = Vec3{0.0f, 0.0f, 1.0f};
            fx.spreadDeg = 60.0f;
            fx.speedMin = 5.0f;
            fx.speedMax = 15.0f;
            fx.gravity = Vec3{0.0f, 0.0f, -5.0f};
            fx.damping = 0.3f;
            fx.sizeStart = 25.0f;
            fx.sizeEnd = 45.0f;
            fx.colorStart[0] = 0.75f; fx.colorStart[1] = 0.70f; fx.colorStart[2] = 0.62f; fx.colorStart[3] = 0.35f;
            fx.colorEnd[0]   = 0.75f; fx.colorEnd[1]   = 0.70f; fx.colorEnd[2]   = 0.62f; fx.colorEnd[3]   = 0.0f;
            fx.blend = rhi::BlendMode::PremultipliedAlpha;
            constexpr u64 kDustCloudEffectId = 0x50415254'44550001ull;   // arbitrary, non-zero
            particles::particleEffects().set(kDustCloudEffectId, fx);

            scene::World& world = scene::World::instance();
            Transform xf;
            xf.position = occluderCube.pos;
            const scene::Entity emitter = world.create("ParticleTest.DustCloud", scene::kInvalidEntity, xf);
            if (auto* emitterComp = static_cast<scene::CParticleEmitter*>(
                    world.addComponent(emitter, scene::kComponentParticleEmitter))) {
                emitterComp->effect = kDustCloudEffectId;
            }
            AVER_INFO("[Particles] --particle-test: dust cloud entity {} around effect 0x{:016X}",
                      emitter, kDustCloudEffectId);

            // A second emitter, DECIDED 4's own proof of the opposite half of the seam: a small
            // stream of embers (an effect that IS its own light source -- see ParticleEffect::
            // receivesGI's own comment), receivesGI = false, additive, beside the cube rather than
            // inside the dust so the two never overlap in one screenshot. Same GI seam, same run,
            // same --no-particle-gi toggle -- if the dust cloud's brightness changes between the two
            // screenshots and this does not, that difference is the seam working, not a coincidence
            // of which effect happened to be lit.
            particles::ParticleEffect emberFx;
            emberFx.shape = particles::EmitterShape::Sphere;
            emberFx.shapeSize = Vec3{10.0f, 0.0f, 0.0f};
            emberFx.emissionRate = 80.0f;
            emberFx.burstCount = 0;
            emberFx.maxParticles = 200;
            emberFx.lifetimeMin = 1.0f;
            emberFx.lifetimeMax = 1.6f;
            emberFx.direction = Vec3{0.0f, 0.0f, 1.0f};
            emberFx.spreadDeg = 35.0f;
            emberFx.speedMin = 40.0f;
            emberFx.speedMax = 80.0f;
            emberFx.gravity = Vec3{0.0f, 0.0f, -25.0f};
            emberFx.damping = 0.1f;
            emberFx.sizeStart = 22.0f;
            emberFx.sizeEnd = 6.0f;
            emberFx.colorStart[0] = 1.0f; emberFx.colorStart[1] = 0.55f; emberFx.colorStart[2] = 0.12f; emberFx.colorStart[3] = 1.0f;
            emberFx.colorEnd[0]   = 1.0f; emberFx.colorEnd[1]   = 0.15f; emberFx.colorEnd[2]   = 0.02f; emberFx.colorEnd[3] = 0.0f;
            emberFx.blend = rhi::BlendMode::Additive;
            emberFx.receivesGI = false;   // DECIDED 4: an ember is its own light source
            constexpr u64 kEmberEffectId = 0x50415254'45420001ull;   // arbitrary, non-zero
            particles::particleEffects().set(kEmberEffectId, emberFx);

            // High above the dust cloud's own top (Z 50+90=140) and well clear of its X/Y footprint,
            // so the two effects never overlap in the frame -- rising embers over a settling dust
            // column, not a gun effect (DECIDED, and see this block's own opening comment).
            //
            // OPEN SKY, NO OCCLUDER BEHIND IT -- and that is deliberate, not incidental: this exact
            // placement is what first exposed the transparent-pass/sky ordering bug this slice found
            // and fixed (see D3D12Device::endFrame's own comment on the reorder). Before that fix, a
            // particle not also backed by real opaque depth was silently overdrawn by the sky pass's
            // own opaque, depth-EQUAL-clear fill -- confirmed by reprojecting this emitter's own
            // particle positions into NDC and finding them well inside the visible range regardless,
            // and reproduced with the dust cloud above disabled so only this emitter drew. Leaving it
            // here, rather than moving it back beside the cube, is itself part of the proof: an
            // emitter with nothing opaque behind it is the common case for smoke, snow, rain and mist,
            // not the exception, and it has to render correctly without one.
            Transform emberXf;
            emberXf.position = occluderCube.pos + Vec3{0.0f, 0.0f, 220.0f};
            const scene::Entity emberEmitter =
                world.create("ParticleTest.Embers", scene::kInvalidEntity, emberXf);
            if (auto* emberComp = static_cast<scene::CParticleEmitter*>(
                    world.addComponent(emberEmitter, scene::kComponentParticleEmitter))) {
                emberComp->effect = kEmberEffectId;
            }
            AVER_INFO("[Particles] --particle-test: ember entity {} around effect 0x{:016X} (receivesGI=false)",
                      emberEmitter, kEmberEffectId);
        }
        // --particle-stress <N> <M>: VERIFICATION-ONLY test content for the parity-and-price
        // adversarial pass, not part of any particles slice. One fog/mist effect (DECIDED 3: effects
        // are shared, edited once) referenced by N emitter entities laid out on a grid, each capped
        // at M particles, so emitter count and per-emitter particle count can each be varied
        // independently to price the system. No weapon vocabulary, no occluder (this content exists
        // to measure cost, not to re-demonstrate the depth test --particle-test already proved).
        else if (particleStressEmitters_ > 0) {
            objects_.clear();
            sel_ = -1;

            particles::ParticleEffect fx;
            fx.shape = particles::EmitterShape::Box;
            fx.shapeSize = Vec3{350.0f, 60.0f, 90.0f};
            fx.emissionRate = 150.0f;   // a steady fog/mist emission rate, not a weapon effect
            fx.maxParticles = static_cast<u32>(particleStressMaxParticles_ > 0 ? particleStressMaxParticles_ : 400);
            // PRICE MEASUREMENT: burst the whole cap on frame 1 so steady-state population (and its
            // CPU/GPU cost) is reached immediately, instead of waiting emissionRate-many seconds for
            // the accumulator to fill it -- a --particle-stress-only choice, not authored-effect data.
            fx.burstCount = fx.maxParticles;
            // PRICE MEASUREMENT: a long, near-constant lifetime keeps the burst-filled population
            // steady for the whole capture window instead of decaying mid-run (a short 3-5s lifetime
            // dies out inside a few hundred frames and contaminates the price with a shrinking
            // population) -- again a stress-harness-only choice, not authored-effect data.
            fx.lifetimeMin = 120.0f;
            fx.lifetimeMax = 120.0f;
            fx.direction = Vec3{0.0f, 0.0f, 1.0f};
            fx.spreadDeg = 60.0f;
            fx.speedMin = 5.0f;
            fx.speedMax = 15.0f;
            fx.gravity = Vec3{0.0f, 0.0f, -5.0f};
            fx.damping = 0.3f;
            fx.sizeStart = 25.0f;
            fx.sizeEnd = 45.0f;
            fx.colorStart[0] = 0.75f; fx.colorStart[1] = 0.70f; fx.colorStart[2] = 0.62f; fx.colorStart[3] = 0.35f;
            fx.colorEnd[0]   = 0.75f; fx.colorEnd[1]   = 0.70f; fx.colorEnd[2]   = 0.62f; fx.colorEnd[3]   = 0.0f;
            fx.blend = rhi::BlendMode::PremultipliedAlpha;
            constexpr u64 kStressEffectId = 0x50415254'53545301ull;   // "PART" + "STS\1", arbitrary
            particles::particleEffects().set(kStressEffectId, fx);

            scene::World& world = scene::World::instance();
            const int n = particleStressEmitters_;
            const int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<f64>(n))));
            const f32 spacing = 140.0f;
            for (int idx = 0; idx < n; ++idx) {
                const int col = idx % cols;
                const int row = idx / cols;
                Transform xf;
                xf.position = Vec3{600.0f,
                                    (static_cast<f32>(col) - static_cast<f32>(cols - 1) * 0.5f) * spacing,
                                    50.0f + static_cast<f32>(row) * spacing};
                const scene::Entity emitter =
                    world.create("ParticleStress.Emitter", scene::kInvalidEntity, xf);
                if (auto* stressComp = static_cast<scene::CParticleEmitter*>(
                        world.addComponent(emitter, scene::kComponentParticleEmitter))) {
                    stressComp->effect = kStressEffectId;
                }
            }
            AVER_INFO("[Particles] --particle-stress: {} emitter(s), {} max particles each, effect 0x{:016X}",
                      n, fx.maxParticles, kStressEffectId);

            // --particle-stress2: VERIFICATION-ONLY, adds a second, differently-blended (additive)
            // small sparks-like emitter alongside the box mist above, so the price measurement can
            // also cover a scene mixing both blend pipelines in one frame, not just one.
            if (particleStressSecondEmitter_) {
                particles::ParticleEffect fx2;
                fx2.shape = particles::EmitterShape::Sphere;
                fx2.shapeSize = Vec3{10.0f, 0.0f, 0.0f};
                fx2.emissionRate = 80.0f;
                fx2.burstCount = 0;
                fx2.maxParticles = 200;
                fx2.lifetimeMin = 1.0f;
                fx2.lifetimeMax = 1.6f;
                fx2.direction = Vec3{0.0f, 0.0f, 1.0f};
                fx2.spreadDeg = 35.0f;
                fx2.speedMin = 40.0f;
                fx2.speedMax = 80.0f;
                fx2.gravity = Vec3{0.0f, 0.0f, -25.0f};
                fx2.damping = 0.1f;
                fx2.sizeStart = 22.0f;
                fx2.sizeEnd = 6.0f;
                fx2.colorStart[0] = 1.0f; fx2.colorStart[1] = 0.55f; fx2.colorStart[2] = 0.12f; fx2.colorStart[3] = 1.0f;
                fx2.colorEnd[0]   = 1.0f; fx2.colorEnd[1]   = 0.15f; fx2.colorEnd[2]   = 0.02f; fx2.colorEnd[3] = 0.0f;
                fx2.blend = rhi::BlendMode::Additive;
                fx2.receivesGI = false;
                constexpr u64 kStressEffect2Id = 0x50415254'53545302ull;
                particles::particleEffects().set(kStressEffect2Id, fx2);

                Transform xf2;
                xf2.position = Vec3{600.0f, 0.0f, 270.0f};
                const scene::Entity emitter2 =
                    world.create("ParticleStress.Emitter2", scene::kInvalidEntity, xf2);
                if (auto* c2 = static_cast<scene::CParticleEmitter*>(
                        world.addComponent(emitter2, scene::kComponentParticleEmitter))) {
                    c2->effect = kStressEffect2Id;
                }
                AVER_INFO("[Particles] --particle-stress: diag second emitter {} effect 0x{:016X}",
                          emitter2, kStressEffect2Id);
            }
        }
#endif

        // --skin-test: the GPU skinning pass against its CPU reference, on this machine's real
        // device. Registered only when asked for, because it costs a waitIdle and exists to be run
        // deliberately -- typically alongside --debug-layer, which is what catches a malformed
        // descriptor as opposed to a wrong number.
        if (skinTest_) {
            skinSelfTest_ = std::make_unique<aver::render::SkinSelfTest>();
            if (skinSelfTest_->init(*e.device())) e.device()->addRenderFeature(skinSelfTest_.get());
            else { AVER_ERROR("[Skin] self-test unavailable on this device"); skinSelfTest_.reset(); }
        }

        // --pt-furnace: does the PATH TRACER conserve energy? It brings its own geometry, its own
        // acceleration structures and its own accumulators, so all the editor supplies is the
        // furnace itself -- setFurnaceTest above is what put SkyAtmosphere::furnaceRadiance on, and
        // the shader reads the environment through the engine's own skyColor().
        if (ptFurnaceTest_) {
            ptFurnace_ = std::make_unique<aver::pt::PtFurnaceTest>();
            if (ptFurnace_->init(*e.device())) e.device()->addRenderFeature(ptFurnace_.get());
            else { AVER_ERROR("[PT] furnace unavailable on this device"); ptFurnace_.reset(); }
        }

        // --pt-scene: the path tracer pointed at the REAL scene instead of the furnace's own private
        // geometry -- a progressive, still-camera reference view that SUPPRESSES the raster scene
        // while registered (see PtSceneView.hpp). It reads the same draw list and camera the raster
        // path already produces (IRenderFeature::submitDraw / IDevice::camera()); everything about
        // what it can and cannot render is documented on PtSceneView.hpp itself (sky plus the one
        // authored sun, static geometry only, flat albedo only). setPtSceneView() (the CLI handler) already set
        // ptSceneViewWantEnabled_; this call is what turns that want into an actual registration,
        // through the SAME path the editor's own Path Tracing settings-page Quality combo uses at any
        // later frame -- see syncPtSceneView()'s own comment for the rest of the design (in
        // particular why it is safe to call outside a frame here, at startup, and every frame from
        // onUpdate() thereafter, but never from inside onRender()).
        syncPtSceneView(e.device());

            tools_.setAutoCompileFlag(autoCompileFlag());
            tools_.setReloader([this](const std::string& binDir, std::string* status) {
                return reloadScripts(binDir, status);
            });
        }
#endif
        tool_ = initialTool_;
        sel_ = 1;
        bool framedByLevel = false;
#if AVER_MODULE_SCENE
        framedByLevel = !levelEntities_.empty();
#endif
        if (!framedByLevel) {
            camPos_ = Vec3{700.0f, 700.0f, 450.0f};
            const Vec3 d = (Vec3{0,0,1} - camPos_).getSafeNormal();
            yaw_ = std::atan2(d.y, d.x);
            pitch_ = std::asin(d.z);
        }
        // Last, so --cam outranks both writers above it: frameCameraOn ran back in loadStartMap,
        // and the default framing is the branch immediately before this.
        if (camOverride_) {
            camPos_ = camPosOverride_;
            pitch_  = pitchOverride_;
            yaw_    = yawOverride_;
        }
    }

    // Advances one frame: MCP commands, camera, gameplay tick, physics, and the render state.
    void onUpdate(Engine& e, const Timestep& t) override {
        // FIRST in the frame, so everything downstream -- the view matrix, the reprojection's
        // gPrevViewProj, the shadow history -- all see one consistent camera for this frame.
        // Latched base yaw rather than accumulating onto yaw_: accumulating would drift with
        // floating-point error and never return exactly to the start, which is the one property
        // this exists to provide.
        if (camWobbleDeg_ != 0.0f && camWobblePeriod_ > 0) {
            if (!camWobbleBased_) { camWobbleBaseYaw_ = yaw_; camWobbleBased_ = true; }
            const f32 phase = 6.2831853f * (f32)(t.frame - 1) / (f32)camWobblePeriod_;
            yaw_ = camWobbleBaseYaw_ + camWobbleDeg_ * 0.01745329252f * std::sin(phase);
        }
        // --pt-scene-toggle-on/-off: verification-only (see the members' own comment). Checked BEFORE
        // syncPtSceneView() so the same onUpdate() that flips the want-flag is the same one that acts
        // on it, rather than costing a whole extra frame of lag for no reason.
        if (ptSceneToggleOnAutoFrames_  > 0 && --ptSceneToggleOnAutoFrames_  == 0) ptSceneViewWantEnabled_ = true;
        if (ptSceneToggleOffAutoFrames_ > 0 && --ptSceneToggleOffAutoFrames_ == 0) ptSceneViewWantEnabled_ = false;
        // BEFORE device_->beginFrame() (see Engine::frameStep()) -- the only safe place to add or
        // remove a render feature. See syncPtSceneView()'s own comment for why.
        syncPtSceneView(e.device());
#if AVER_MODULE_MCP
        // One event per frame: a click needs a press frame and a later release frame to register.
        if (mcp_.listening()) mcp_.pump([this](const mcp::Command& c) { applyMcpCommand(c); });
#endif
        if (sky_.cloudsEnabled) cloudTime_ += t.dt;
        if (showUiDemo_) {
            uiDemoHealth_  = 0.5f + 0.5f * std::sin(uiDemoClock_ * 0.7f);
            uiDemoStamina_ = 0.5f + 0.5f * std::sin(uiDemoClock_ * 1.6f + 1.0f);
            uiDemoScroll_  = std::fmod(uiDemoScroll_ + t.dt * 24.0f, 216.0f);   // px, wraps at 12 rows
            uiDemoClock_  += t.dt;
        }
#if AVER_WITH_IMGUI
        if (e.device()->uiActive()) {
            // Must stay outside the ImGui frame: the font atlas may not be touched inside one.
            const f32 dpi = e.window() ? e.window()->dpiScale() : dpi_;
            if (std::fabs(dpi - dpi_) > 0.01f) {
                AVER_INFO("[Sandbox] DPI changed {:.2f} -> {:.2f}, re-rasterising the UI font", dpi_, dpi);
                applyDpi(dpi);
            }
        }
        if (e.device()->uiActive() && !browserActive_ && !gameHasInput()) {
            const ImGuiIO& io = ImGui::GetIO();
            const bool overUI = !levelHovered_ || !inViewport(io.MousePos.x, io.MousePos.y);
            if (inputProbe_ && (ImGui::GetFrameCount() % 30) == 0)
                AVER_INFO("[input-probe] mouse=({},{}) wantCaptureMouse={} hovered={} inViewport={} "
                          "focused={} -> overUI={} | flying={} cam=({:.0f},{:.0f},{:.0f}) yaw={:.2f}",
                          (int)io.MousePos.x, (int)io.MousePos.y, (int)io.WantCaptureMouse,
                          (int)levelHovered_, (int)inViewport(io.MousePos.x, io.MousePos.y),
                          (int)levelFocused_, (int)overUI, (int)flying_,
                          camPos_.x, camPos_.y, camPos_.z, yaw_);

            // Right mouse enters fly mode: look plus WASD/QE, cursor hidden but never warped.
            if (ImGui::IsMouseClicked(1) && !overUI) flying_ = true;
            if (!io.MouseDown[1]) flying_ = false;
            if (flying_) ImGui::SetMouseCursor(ImGuiMouseCursor_None);

            if (flying_) {
                yaw_   += io.MouseDelta.x * lookSpeed_;
                pitch_ -= io.MouseDelta.y * lookSpeed_;
                pitch_ = pitch_ < -1.54f ? -1.54f : (pitch_ > 1.54f ? 1.54f : pitch_);
                if (io.MouseWheel != 0.0f) {
                    flySpeed_ *= (1.0f + io.MouseWheel * 0.15f);
                    flySpeed_ = flySpeed_ < 20.0f ? 20.0f : (flySpeed_ > 40000.0f ? 40000.0f : flySpeed_);
                }
            }

            const Vec3 fwd = camForward();
            const Vec3 up{0, 0, 1};
            const Vec3 right = cross(up, fwd).getSafeNormal();

            if (flying_ && !io.WantCaptureKeyboard) {
                const f32 sp = flySpeed_ * t.dt;
                if (ImGui::IsKeyDown(ImGuiKey_W)) camPos_ += fwd * sp;
                if (ImGui::IsKeyDown(ImGuiKey_S)) camPos_ -= fwd * sp;
                if (ImGui::IsKeyDown(ImGuiKey_D)) camPos_ += right * sp;
                if (ImGui::IsKeyDown(ImGuiKey_A)) camPos_ -= right * sp;
                if (ImGui::IsKeyDown(ImGuiKey_E)) camPos_ += up * sp;
                if (ImGui::IsKeyDown(ImGuiKey_Q)) camPos_ -= up * sp;
            } else if (!overUI) {
                if (io.MouseWheel != 0.0f) camPos_ += fwd * io.MouseWheel * (flySpeed_ * 0.15f);
                if (io.MouseDown[2]) { camPos_ -= right * io.MouseDelta.x * 0.02f; camPos_ += up * io.MouseDelta.y * 0.02f; }
            }
            // F frames the selection at a distance derived from its radius.
            if (levelFocused_ && !io.WantCaptureKeyboard &&
                keybinds_.pressed(editor::CommandId::ViewFrameSelected, io) && anySelected()) {
                EditXform x;
                if (selectedXform(x)) {
                    const f32 r = selectedRadius();
                    const f32 d = std::fmax(50.0f, r / std::tan(radians(30.0f)) * 1.6f);
                    camPos_ = x.pos - fwd * d;
                    flySpeed_ = std::fmax(flySpeed_, r * 0.4f);
#if AVER_MODULE_SCENE
                    chunkStreamHaveLastPos_ = false;   // teleport; see frameCameraOn for why
#endif
                }
            }
        }
#endif
#if AVER_MODULE_SCRIPTING
        scripts_.update(t.dt);
#endif
#if AVER_MODULE_FRAMEWORK
        const bool interactive = maxFrames_ == 0 && !playTest_;
        // THE #if IS NOT REDUNDANT WITH uiActive(). uiActive() is a RUNTIME question -- "is an ImGui
        // frame open right now" -- and it answers false on a UI-less build, which is why this block
        // was correct at runtime and still failed to COMPILE without ImGui: the ImGuiIO/ImGui::
        // names below have to exist for the translation unit regardless of what the branch decides.
        // scripts/module-matrix.ps1's no-ui and d3d12-off rows both failed here (d3d12-off because
        // ImGui enters the tree from the D3D12 backend, so turning that backend off removes it too).
        // Guarding the whole block rather than each call keeps the mouse/keyboard capture policy in
        // one piece; a UI-less build has no ImGui to arbitrate capture with in the first place.
#if AVER_WITH_IMGUI
        if (e.device()->uiActive() && interactive) {
            // Clicking the viewport puts the mouse back in the game. Tested before wantCapture below.
            {
                const ImGuiIO& mio = ImGui::GetIO();
                if (playSessionActive() && releasedByUser_ && ImGui::IsMouseClicked(0) &&
                    !mio.WantCaptureMouse && inViewport(mio.MousePos.x, mio.MousePos.y))
                    releasedByUser_ = false;
            }
            const bool wantCapture = playSessionActive() && !releasedByUser_;
            if (keybinds_.pressed(editor::CommandId::PlayReleaseMouse, ImGui::GetIO()) && playSessionActive())
                releasedByUser_ = !releasedByUser_;
            // ESCAPE STOPS PLAY-IN-EDITOR, the same action as clicking Stop. Checked here rather
            // than in pushInput: this runs once whether or not the mouse is captured, and it must
            // win over the game seeing the keypress -- a script reading Escape for its own pause
            // menu should not also race the editor for what the key means.
            // Escape ends a drone stand-in too. Play started it, so Play's exit has to end it, or
            // the only way out is a menu item the user has no reason to think is involved.
            if (keybinds_.pressed(editor::CommandId::PlayStop, ImGui::GetIO()) && (playSessionActive() || dronePlayActive()))
                stopPlay();
            if (!playSessionActive()) releasedByUser_ = false;
            setMouseCaptured(wantCapture && !ImGui::GetIO().WantTextInput);
        }
#else
        (void)interactive;   // no ImGui to arbitrate mouse/keyboard capture with
#endif
        pollCapturedMouse();
        pushInput(e.device()->uiActive());
        // The UI frame opens before gameplay ticks, because ticking is when a game draws its HUD.
#if AVER_MODULE_SCRIPTING
        // --hud-preview <n>: name every HUD once, then preview one over the level viewport.
        if (hudTest_ >= 0 && !hudTestReported_ && scripts_.ready()) {
            hudTestReported_ = true;
            const i32 n = scripts_.hudCount();
            AVER_INFO("[HUD] {} declared", n);
            for (i32 i = 0; i < n; ++i) AVER_INFO("[HUD]   {}: '{}'", i, scripts_.hudName(i));
            if (hudTest_ >= n) AVER_WARN("[HUD] no HUD at index {}", hudTest_);
        }
        if (hudTest_ >= 0 && hudTest_ < scripts_.hudCount())
            setHudPreview(hudTest_, vpX_, vpY_, vpW_, vpH_);
#endif
        if (hudPreviewActive()) aver_ui_begin_frame(hudRectX_, hudRectY_, hudRectW_, hudRectH_);
        else                    aver_ui_begin_frame(vpX_, vpY_, vpW_, vpH_);
#if AVER_MODULE_SCRIPTING
        if (hudPreviewActive()) scripts_.hudDraw(hudPreviewIndex_, t.dt);
#endif
        maybeSpawnTestActor();
        maybePlayTest();
        // The tick groups bracket the physics step: PrePhysics -> Physics -> PostPhysics.
        if (!spawnTestClass_.empty() || aver_fw_play_state() == AVER_FW_PLAY_PLAYING) {
            aver_fw_tick(AVER_FW_TICK_PRE_PHYSICS, t.dt);
#if AVER_MODULE_PHYSICS
            aver_phys_step(t.dt);
#endif
            aver_fw_tick(AVER_FW_TICK_PHYSICS, t.dt);
            aver_fw_tick(AVER_FW_TICK_POST_PHYSICS, t.dt);
        }
#endif
#if AVER_MODULE_SCENE
        // Retires deferred destroys and propagates world matrices once, after gameplay and before onRender.
        // The animation clock, UNCONDITIONALLY and not from the gameplay tick above. Those tick
        // groups are gated on PLAYING, so hanging this off them would freeze every preview the
        // moment the editor was not in play -- which is exactly when somebody is looking at one.
        anim::animSystem().tick(scene::World::instance(), t.dt);
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
        // Same "unconditionally, not from the gameplay tick" reasoning as the animation clock just
        // above: a preview outside Play mode should still show its effects playing.
        // VERIFICATION-ONLY: the steady_clock pair brackets ONLY the CPU simulation call, isolating
        // it from the GPU draw the --frame-time GPU-marker report already separately accounts for
        // ("Aver.Particles" in that report is the DRAW pass; this is the sim). See
        // particleTickAccumSec_'s own comment.
        {
            const auto tickStart = std::chrono::steady_clock::now();
            particles::particleSystem().tick(scene::World::instance(), t.dt);
            particleTickAccumSec_ += std::chrono::duration<f64>(std::chrono::steady_clock::now() - tickStart).count();
            ++particleTickFrames_;
        }
#endif
        // AFTER the tick and BEFORE anything draws: update() is what creates the per-entity skin
        // targets the draw pass is about to ask for, and what copies this frame's matrices out of
        // the AnimSystem -- whose skinning() is only valid until the next tick.
        if (skinnedScene_)
            skinnedScene_->update(scene::World::instance(), anim::animSystem(), *e.device());
        // --drone: switches the graph-driven drone on N frames in, on its own, mirroring
        // --chunk-stream immediately below so a --frames capture run can prove it without a human
        // clicking Window > Drone.
        if (droneAutoFrames_ > 0 && --droneAutoFrames_ == 0) setDroneEnabled(true);
        // --undo-test: fires runUndoTest() N frames in, then the process exits -- see its own
        // comment for what it proves and why it exits rather than returning. runUndoTest() itself
        // lives inside the same `#if AVER_WITH_IMGUI` block as deleteSelection/copySelection/
        // pasteClipboard/duplicateSelection (the commands it is proving), so this call site needs
        // the same guard.
#if AVER_WITH_IMGUI
        if (undoTestAutoFrames_ > 0 && --undoTestAutoFrames_ == 0) runUndoTest(e);
        if (keybindTestAutoFrames_ > 0 && --keybindTestAutoFrames_ == 0) runKeybindPersistTest(keybindTestMode_);
#endif
#if AVER_MODULE_SCRIPTING
        // Ticks the graph-driven drone, if one is live. Runs BEFORE chunk streaming below so this
        // frame's drone position/velocity are what chunk streaming's extra StreamSource (and the log
        // line right under it) see -- not last frame's.
        if (droneEntity_ != scene::kInvalidEntity && droneGraphLoaded_) {
            droneTimeSeconds_ += t.dt;
            scripts_.graphTick(static_cast<i32>(droneEntity_), droneTimeSeconds_);
            scene::World& dw = scene::World::instance();
            if (dw.valid(droneEntity_)) {
                dronePos_ = dw.localTransform(droneEntity_).position;
                const f32 droneInvDt = t.dt > 1e-6f ? 1.0f / t.dt : 0.0f;
                droneVel_ = droneHaveLastPos_ ? (dronePos_ - droneLastPos_) * droneInvDt
                                              : Vec3{0.0f, 0.0f, 0.0f};
                droneLastPos_ = dronePos_;
                droneHaveLastPos_ = true;
                // Greppable proof the drone actually MOVED, across several frames -- not just that it
                // spawned. 30 lines, not chunk streaming's 8: the ask here is a trajectory.
                if (droneLogsLeft_ > 0) {
                    --droneLogsLeft_;
                    AVER_INFO("[Drone] t={:.3f}s pos=({:.1f},{:.1f},{:.1f}) vel=({:.1f},{:.1f},{:.1f})cm/s",
                              droneTimeSeconds_, dronePos_.x, dronePos_.y, dronePos_.z,
                              droneVel_.x, droneVel_.y, droneVel_.z);
                }
            }
        }
        // GRAPH-AS-CLASS instances -- UNGATED on Play state, same reasoning as the drone tick just
        // above and as modules/runtime.game/src/GameApp.cpp's own tickGraphClassInstances call site:
        // a graph-only project never calls aver_fw_begin_play (no C# GameMode to find), so gating this
        // on aver_fw_play_state() would make a class-placed graph instance's OnTick never run at all
        // while merely browsing a level -- see ScriptHost::tickGraphClassInstances' own comment.
        scripts_.tickGraphClassInstances(t.dt);
#endif
        // --chunk-stream: switches streaming on N frames in, on its own, so a --frames capture run
        // can prove it happened without a human clicking Window > Chunk Streaming.
        if (chunkStreamAutoFrames_ > 0 && --chunkStreamAutoFrames_ == 0) setChunkStreamingEnabled(true);
        // Chunk streaming, if switched on. Runs here so it sees THIS frame's camPos_ (the WASD/fly
        // block above has already finalized it) and so its evictions land in the flush() right below
        // -- not runs unconditionally, i.e. also while just idling in the editor and not in Play
        // mode, which is deliberate: a designer flying around outside Play is exactly who this
        // feature is for.
        if (chunkWorld_) {
            const f32 invDt = t.dt > 1e-6f ? 1.0f / t.dt : 0.0f;
            // First frame after enabling (or after any camera teleport -- see frameCameraOn and the
            // F-key jump above) reports zero velocity: differencing against a stale/teleported-from
            // position would ask the streamer to prefetch a corridor toward nowhere real.
            const Vec3 vel = chunkStreamHaveLastPos_ ? (camPos_ - chunkStreamLastCamPos_) * invDt
                                                      : Vec3{0.0f, 0.0f, 0.0f};
            chunkStreamLastCamPos_ = camPos_;
            chunkStreamHaveLastPos_ = true;

            // The other half of the CPU/GPU question above, and the answer to a specific suspicion:
            // the streamer reports a pending backlog in the hundreds for the first few seconds, which
            // reads like it is generating every frame forever. It is not -- the backlog drains and
            // this settles to 0.7ms at pending=0. Worth measuring rather than assuming, since
            // "streaming is really laggy" was a live complaint and this is where it would show.
            const auto tStream0 = std::chrono::steady_clock::now();

            std::vector<i32> freed;
#if AVER_MODULE_SCRIPTING
            if (droneEntity_ != scene::kInvalidEntity && droneGraphLoaded_) {
                // Optional part 4: the drone flying is what pulls chunks in too, not just the camera
                // -- both are StreamSource entries, so either one moving keeps its own corridor
                // resident. See ChunkWorld::update's std::vector<StreamSource> overload.
                const std::vector<world::StreamSource> sources = {
                    world::StreamSource{camPos_, vel},
                    world::StreamSource{dronePos_, droneVel_},
                };
                chunkStreamStats_ = chunkWorld_->update(scene::World::instance(), sources, t.dt, &freed);
                // Every additional density field streams on the SAME sources and the same dt, each
                // against its own radius. Stats are summed rather than replaced: "resident chunks"
                // means the whole world, and reporting only the primary's would under-report by
                // exactly the strata this feature exists to add.
                for (auto& extra : chunkWorldsExtra_) {
                    if (!extra) continue;
                    accumulateStreamStats(chunkStreamStats_,
                                          extra->update(scene::World::instance(), sources, t.dt, &freed));
                }
            } else
#endif
            {
                chunkStreamStats_ = chunkWorld_->update(scene::World::instance(), camPos_, vel, t.dt, &freed);
                for (auto& extra : chunkWorldsExtra_) {
                    if (!extra) continue;
                    accumulateStreamStats(chunkStreamStats_,
                                          extra->update(scene::World::instance(), camPos_, vel, t.dt, &freed));
                }
            }
            {
                const f64 streamMs = std::chrono::duration<f64, std::milli>(
                    std::chrono::steady_clock::now() - tStream0).count();
                if ((chunkStreamReports_ & (chunkStreamReports_ + 1)) == 0)
                    AVER_INFO("[Sandbox] chunk stream update {:.1f}ms on the main thread "
                              "(resident {}, pending {})", streamMs,
                              chunkStreamStats_.residentChunks, chunkStreamStats_.pendingLoads);
                ++chunkStreamReports_;
            }
#if AVER_MODULE_PHYSICS
            for (const i32 b : freed) if (b >= 0) aver_phys_remove_body(b);
#endif
            // Greppable proof for a headless run: "[ChunkWorld]" lines for the first few frames that
            // actually loaded or evicted something, then it quiets down so a long capture is not
            // flooded once steady state is reached.
            if (chunkStreamLogsLeft_ > 0 &&
                (chunkStreamStats_.loadedThisUpdate > 0 || chunkStreamStats_.evictedThisUpdate > 0)) {
                --chunkStreamLogsLeft_;
                AVER_INFO("[ChunkWorld] loaded={} evicted={} resident={}chunks/{}entities/{}tris "
                          "pending={} failed={} totalLoads={} vel=({:.0f},{:.0f},{:.0f})cm/s",
                          chunkStreamStats_.loadedThisUpdate, chunkStreamStats_.evictedThisUpdate,
                          chunkStreamStats_.residentChunks, chunkStreamStats_.residentEntities,
                          residentTriangleCount(),
                          chunkStreamStats_.pendingLoads, chunkStreamStats_.failedLoads,
                          chunkStreamStats_.totalLoads, vel.x, vel.y, vel.z);
            }
        }
        scene::World::instance().flush();
#endif
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
        drivePlayCamera();
#endif
#if AVER_MODULE_VOXI
        e.device()->setMeshShaders(voxi::Renderer::get().settings().meshShaders);
#else
        e.device()->setMeshShaders(msOverride_);
#endif
#if AVER_MODULE_VOXI
        // Voxi owns the AA setting; push it to the device when it changes (rebuilds targets+PSOs).
        if (voxi::Renderer::get().consumeMsaaDirty())
            e.device()->setSampleCount(static_cast<u32>(voxi::Renderer::get().settings().msaa));

        if (voxiAttached_) {
            const voxi::Settings& vs = voxi::Renderer::get().settings();
            const f32 c[3] = {giCenter_.x, giCenter_.y, giCenter_.z};
            voxiRenderer_.setSettings(vs);
            voxiRenderer_.setVolume(c, giExtent_);
            voxiRenderer_.setDebugView(giDebugView_);
            // --no-gi-cone: see setGiConeTraceOff's own comment. Applied every frame, same as
            // setDebugView beside it, so the toggle takes effect the instant the flag is set rather
            // than only at attach time.
            voxiRenderer_.setConeTraceEnabled(!giConeTraceOff_);
            const Vec3 sd = Vec3{sky_.sunDirection[0], sky_.sunDirection[1],
                                 sky_.sunDirection[2]}.getSafeNormal();
            if (sunAngle_ > 0.0f) sky_.sunAngularDiameterDeg = sunAngle_;
            // Direction only. sunColor_ and sunAmbient_ reach the shaders through sky_ below
            // (:1136, :1141) and the device's frame constants -- passing them here as well was
            // storing a second copy nothing read.
            voxiRenderer_.setSunDirection(&sd.x);
        }
#endif
        // Confine the scene to the dockspace's central node (latched by buildUI last frame).
        e.device()->setViewportRect((u32)vpX_, (u32)vpY_, (u32)std::fmax(1.0f, vpW_), (u32)std::fmax(1.0f, vpH_));

        const Vec3 fwd = camForward();
        const f32 aspect = viewAspect();
        const Mat4 view = Mat4::lookAtLH(camPos_, camPos_ + fwd, Vec3{0,0,1});
        const f32 zNear = 2.0f, zFar = 200000.0f;   // cm
        const Mat4 proj = Mat4::perspectiveLH(radians(60.0f), aspect, zNear, zFar);
        const Mat4 viewProj = view * proj;
        const Mat4 invVP = viewProj.inverse();
        e.device()->setCamera(&viewProj.m[0][0], &invVP.m[0][0], &camPos_.x);
        invVP_ = invVP; viewProj_ = viewProj; eye_ = camPos_;

        // A loaded level's own FOG record wins over the editor default.
        f32 fog = fogDensity_;
#if AVER_MODULE_SCENE
        if (hasLevelFog_) fog = levelFog_;
        // OPT-IN, and overrides whatever the level/slider said while it is on: match fog density to
        // the streaming load boundary instead. See fogDensityForOpacityAt and the member comment on
        // matchFogToStreamRadius_ for the derivation and the honesty caveat -- this makes the world
        // visibly foggier, on purpose, and only when asked for.
        if (matchFogToStreamRadius_ && chunkWorld_) {
            const world::StreamSettings& st = chunkWorld_->settings().stream;
            const f32 boundaryCm = static_cast<f32>(st.loadRadius) * static_cast<f32>(st.chunkSizeCm);
            const f32 matched = fogDensityForOpacityAt(boundaryCm, fogMatchTargetOpacity_);
            if (matched > 0.0f) fog = matched;
        }
#endif
        // FROZEN: sunDirection stays unnormalised here -- the shaders normalise it.
        sky_.enabled = true;
        for (int i = 0; i < 3; ++i) {
            sky_.sunColor[i] = sunColor_[i];
            sky_.zenith[i]   = skyZenith_[i];
            sky_.horizon[i]  = skyHorizon_[i];
            sky_.fogColor[i] = fogColor_[i];
        }
        sky_.skyLightIntensity = sunAmbient_;
        sky_.fogDensity = fog;
        sky_.cloudTime = cloudTime_;
        e.device()->setSkyAtmosphere(sky_);
        // Outside the viewport rect is editor chrome, not sky.
        e.device()->setClearColor(0.055f, 0.055f, 0.062f, 1);
        e.device()->setPostProcess(post_);
    }

#if AVER_MODULE_PBR
    // Uploads the texture a material reference names and returns its handle. 0 keeps the slot's fallback.
    static rhi::TextureHandle resolveMaterialTexture(const pbr::TextureRef& ref, pbr::TextureSlot slot,
                                                     void* user) {
        auto* self = static_cast<SandboxApp*>(user);
        if (!self || !self->textureFactory_) return 0;

        const std::string path = self->resolveAssetPath(ref);
        if (path.empty()) {
            AVER_WARN("[Material] texture id 0x{:016X} is not in the content index; slot '{}' keeps "
                      "its fallback", ref.id, pbr::MaterialLibrary::textureSlotName(slot));
            return 0;
        }

        // The slot decides the colour space, never the filename.
        assets::TextureUsage usage = assets::TextureUsage::Data;
        switch (slot) {
            case pbr::TextureSlot::BaseColor:
            case pbr::TextureSlot::Emissive:  usage = assets::TextureUsage::Colour;    break;
            case pbr::TextureSlot::Normal:    usage = assets::TextureUsage::NormalMap; break;
            default:                          usage = assets::TextureUsage::Data;      break;
        }

        std::string err;
        assets::TextureUploadInfo info;
        const rhi::TextureHandle h = assets::uploadTexture(*self->textureFactory_, path, usage, &err, &info);
        if (!h) {
            AVER_WARN("[Material] {} — slot '{}' keeps its fallback", err,
                      pbr::MaterialLibrary::textureSlotName(slot));
            return 0;
        }
        AVER_INFO("[Material] {} -> {}x{}, {} mips ({} KB) for slot '{}'", path, info.width, info.height,
                  info.mips, info.bytes / 1024, pbr::MaterialLibrary::textureSlotName(slot));
        return h;
    }

    // Returns where an asset reference points on this machine, or empty when it cannot be resolved.
    std::string resolveAssetPath(const pbr::TextureRef& ref) const {
        if (!ref.path.empty()) {
            const std::string& p = ref.path;
            const bool absolute = p.size() > 1 && (p[1] == ':' || p[0] == '\\' || p[0] == '/');
            if (absolute) return p;
            const std::string content = project_.contentDir();
            if (!content.empty()) {
                const std::string full = content + "\\" + p;
                std::error_code ec;
                if (std::filesystem::exists(full, ec)) return full;
            }
            return p;
        }
        if (ref.id) {
            const auto it = contentIndex_.find(ref.id);
            if (it != contentIndex_.end()) return it->second;
        }
        return {};
    }

#endif  // AVER_MODULE_PBR -- the material-specific helpers end here.

#if AVER_MODULE_VOXI && AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // ---- particles DECIDED 4: the two halves of particles::ParticleRenderer::GiSeam ----
    //
    // Both static (like resolveMaterialTexture/depthProxyLookup just above and below): the ONLY
    // members these read are voxiRenderer_'s, reached through `user`, never `this` implicitly -- so
    // installing them below as `&SandboxApp::particleGiPrepare, this` is the exact same idiom
    // setDepthProxy/setTextureResolver already use. modules/particles never sees this file, this
    // class, or the fact that "voxi" is the name on the other end of its GiSeam -- these two
    // functions are that entire boundary.

    // PIPELINE-BUILD TIME half. giShaderPrelude()/giShaderDefines() are pure functions of the
    // register numbers ParticleRenderer::buildPipelines hands in -- see VoxiGiShaders.hpp -- so this
    // never actually needs to dereference `user`; it exists to keep the signature uniform with
    // particleGiBind below, which does.
    static bool particleGiPrepare(u32 srvBase, u32 samplerBase, u32 cbRegister,
                                  std::string* outPrelude, std::string* outDefines, void* user) {
        (void)user;
        if (!outPrelude || !outDefines) return false;
        *outPrelude = voxi::giShaderPrelude();
        *outDefines = voxi::giShaderDefines(srvBase, samplerBase, cbRegister);
        return true;
    }

    // PER-FRAME half. Forwards straight to voxiRenderer_'s own bindGiResources/giFrameConstants --
    // see those methods' own comments for why a null volume/shadow texture or a not-yet-ready Voxi
    // degrades safely rather than needing a readiness check here.
    static void particleGiBind(rhi::IResourceFactory& res, rhi::BindingSetHandle set, u32 srvBase,
                               const void** outCbData, u32* outCbBytes, void* user) {
        auto* self = static_cast<SandboxApp*>(user);
        if (!self || !outCbData || !outCbBytes) return;
        self->voxiRenderer_.bindGiResources(res, set, srvBase);
        *outCbData = self->voxiRenderer_.giFrameConstants();
        *outCbBytes = self->voxiRenderer_.giFrameConstantBytes();
    }
#endif  // AVER_MODULE_VOXI && AVER_MODULE_PARTICLES && AVER_MODULE_SCENE

    // ---- content and mesh loading: NOT material work, and no longer guarded as if it were --------
    //
    // Everything from here to releaseProjectMeshes sat inside the AVER_MODULE_PBR block that opened
    // above, presumably because the content index was first written for `{guid:...}` TEXTURE
    // references. But putting a MESH in the world is not a material concern: with PBR off, the
    // editor lost its asset index, its mesh registry and both resolver callbacks, while every call
    // site kept calling them.

    // Indexes every asset under the project's content root by fnv1a64 of its content-relative path.
    void rebuildContentIndex() {
        contentIndex_.clear();
        const std::string content = project_.contentDir();
        if (content.empty()) return;
        std::error_code ec;
        if (!std::filesystem::exists(content, ec)) return;
        for (std::filesystem::recursive_directory_iterator it(content, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            std::string rel = std::filesystem::relative(it->path(), content, ec).string();
            if (ec || rel.empty()) continue;
            // FROZEN: the id hashes the forward-slash spelling, matching C# Assets.ObjectIdOf.
            for (char& c : rel) if (c == '\\') c = '/';
            contentIndex_[fnv1a64(std::string_view(rel))] = it->path().string();
        }
        AVER_INFO("[Content] indexed {} asset(s) under {}", contentIndex_.size(), content);
        // The anim system does its own file discovery through this, and caches by id -- so a
        // re-index has to drop what it cached or a moved asset keeps resolving to the old path.
        // Aver.Anim.Scene (anim::animSystem) is built only under AVER_MODULE_SCENE (root CMakeLists,
        // modules/anim.scene sits inside that if()), so these two calls need their own guard even
        // though the rest of this function is mesh-loading work that has nothing to do with a
        // material or a scene and must stay unguarded.
#if AVER_MODULE_SCENE
        anim::animSystem().clear();
        anim::animSystem().setResolver(&SandboxApp::resolveAnimAsset, this);
#endif
    }

    // Maps an asset ObjectId to a path for aver::anim::AnimSystem. A plain function pointer because
    // that is what the system takes: asset discovery is the host's business, not the sampler's.
    static std::string resolveAnimAsset(u64 id, void* user) {
        auto* self = static_cast<SandboxApp*>(user);
        if (!self) return {};
        const auto it = self->contentIndex_.find(id);
        return it == self->contentIndex_.end() ? std::string() : it->second;
    }

    // Maps a mesh ObjectId to the handle the scene pass would draw, for aver::render::SkinnedScene.
    // The SAME table the draw pass uses, deliberately: a skin target built from a different upload
    // than the one on screen would be a rig skinning geometry nobody can see.
    //
    // Guarded: its only caller is skinnedScene_->setResolvers(...), itself inside #if AVER_MODULE_SCENE
    // (skinnedScene_ does not exist otherwise), and the body reads sceneMeshes_, which is a member
    // declared only under the same guard.
#if AVER_MODULE_SCENE
    static rhi::MeshHandle resolveSceneMesh(u64 id, void* user) {
        auto* self = static_cast<SandboxApp*>(user);
        if (!self) return 0;
        const auto it = self->sceneMeshes_.find(id);
        return it == self->sceneMeshes_.end() ? 0 : it->second;
    }
#endif

// PBR-ONLY, and stranded outside its guard when the content/mesh helpers moved out of the
// material block: it returns a pbr::MaterialHandle, so the SIGNATURE needs the module, not just
// the body. Its callers -- the level loader and loadProjectMaterials -- are both already guarded.
#if AVER_MODULE_PBR
    // Returns the material a surface token names, loading it on first use. 0 when the project has none.
    pbr::MaterialHandle materialForSurface(const std::string& name) {
        if (name.empty()) return 0;
        const auto cached = materialAssets_.find(name);
        if (cached != materialAssets_.end()) return cached->second;

        pbr::MaterialHandle h = 0;
        const std::string content = project_.contentDir();
        if (!content.empty()) {
            // Built .ocmat under Binaries wins over a hand-authored one under Content.
            const std::string candidates[3] = {
                project_.binariesDir() + "\\Materials\\" + name + ".ocmat",
                content + "\\Materials\\" + name + ".ocmat",
                content + "\\" + name,
            };
            for (const std::string& path : candidates) {
                std::error_code ec;
                if (!std::filesystem::exists(path, ec)) continue;
                pbr::MaterialDesc d;
                fmt::OcMatExtras extras;
                std::string err;
                if (!fmt::loadOcmat(path, d, &extras, &err)) { AVER_WARN("[Material] {}", err); break; }
                h = pbr::MaterialLibrary::get().create(d);
                if (h) AVER_INFO("[Material] '{}' loaded from {}", d.name, path);
                break;
            }
        }
        materialAssets_.emplace(name, h);
        return h;
    }
#endif  // AVER_MODULE_PBR

    // Loads every .ocmesh under the project's Content, keyed by fnv1a64 of its forward-slash relative path.
    void loadProjectMeshes(Engine& e) {
#if AVER_MODULE_SCENE
        const std::string dir = project_.contentDir();
        if (dir.empty()) return;
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec)) return;

        u32 loaded = 0, failed = 0;
        for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            const std::string full = it->path().string();
            if (assetTypeFromPath(full) != AssetType::Mesh) continue;

            std::string rel = std::filesystem::relative(it->path(), dir, ec).string();
            if (ec) continue;
            for (char& c : rel) if (c == '\\') c = '/';

            fmt::OcMeshData md;
            std::string why;
            if (!fmt::loadOcMesh(full, md, &why)) { AVER_WARN("[Mesh] {}", why); ++failed; continue; }

            std::vector<rhi::MeshVertex> verts(md.vertexCount());
            for (u32 i = 0; i < md.vertexCount(); ++i) {
                rhi::MeshVertex& v = verts[i];
                v.px = md.positions[usize(i)*3+0]; v.py = md.positions[usize(i)*3+1]; v.pz = md.positions[usize(i)*3+2];
                v.nx = md.normals[usize(i)*3+0];   v.ny = md.normals[usize(i)*3+1];   v.nz = md.normals[usize(i)*3+2];
                v.u  = md.uvs[usize(i)*2+0];       v.v  = md.uvs[usize(i)*2+1];
            }
            const rhi::MeshHandle h = e.device()->createMesh(verts.data(), (u32)verts.size(),
                                                            md.indices.data(), (u32)md.indices.size());
            if (!h) { AVER_WARN("[Mesh] the device refused '{}'", rel); ++failed; continue; }

            const u64 id = fnv1a64(std::string_view(rel));
            sceneMeshes_[id] = h;
            meshBounds_[id] = {md.boundsMin, md.boundsMax};
            meshTris_[id] = static_cast<u32>(md.indices.size() / 3);
            projectMeshIds_.push_back(id);
            ++loaded;
            AVER_INFO("[Mesh] '{}' -> {} verts, {} indices, lodCount={}, coarserLods={}, meshlets={}",
                      rel, verts.size(), md.indices.size(), md.lodCount(), md.coarserLods.size(),
                      md.meshlets.size());

#if AVER_MODULE_TRIFACTOR
            // The Cook wrote coarser LOD levels for this mesh: build one whole-level MeshHandle per
            // level, ONCE, here at load time -- never per frame. Every level shares `verts` (the SAME
            // vertex buffer, per OcMeshData::coarserLods' own contract), so this only duplicates INDEX
            // data on the GPU, never vertices, and it sidesteps entirely the per-frame-index-upload
            // failure mode the task brief warns about: nothing about the selection pass below writes
            // to a buffer while a frame might still be reading it, because nothing is written per
            // frame at all -- selection only ever CHOOSES among these already-resident handles.
            if (md.lodCount() > 1) {
                MeshLodLadder ladder;
                const u32 levels = md.lodCount();
                ladder.handles.reserve(levels);
                ladder.triCounts.reserve(levels);
                ladder.errorCm.reserve(levels);
                ladder.clusters.resize(levels);

                ladder.handles.push_back(h);
                ladder.triCounts.push_back(meshTris_[id]);
                ladder.errorCm.push_back(0.0f);
                trifactor::buildLevelClusterViews(md, 0, ladder.clusters[0]);

                bool ok = true;
                for (u32 lvl = 1; lvl < levels && ok; ++lvl) {
                    const fmt::OcMeshLod& lod = md.coarserLods[lvl - 1];
                    const rhi::MeshHandle lh = e.device()->createMesh(
                        verts.data(), (u32)verts.size(), lod.indices.data(), (u32)lod.indices.size());
                    if (!lh) {
                        AVER_WARN("[Mesh] '{}' LOD {} refused by the device; ladder truncated at {} level(s)",
                                  rel, lvl, ladder.handles.size());
                        ok = false;
                        break;
                    }
                    ladder.handles.push_back(lh);
                    ladder.triCounts.push_back(trifactor::levelTriangleCount(md, lvl));
                    ladder.errorCm.push_back(trifactor::levelWorldErrorCm(md, lvl));
                    trifactor::buildLevelClusterViews(md, lvl, ladder.clusters[lvl]);
                }
                AVER_INFO("[Mesh] '{}' LOD ladder: {} level(s), {} tris at LOD0 -> {} tris at the coarsest",
                          rel, ladder.handles.size(), ladder.triCounts.front(), ladder.triCounts.back());

                // THE HANDLE THE SHADOW AND VOXEL PASSES WILL DRAW INSTEAD. Those passes are
                // depth-only: they resolve an occluder's silhouette, never its surface, so the
                // detail a coarser level drops is detail they were not going to show. The lit pass
                // is unaffected -- it keeps choosing per instance as it always has.
                //
                // CHOSEN ON WORLD ERROR, NOT ON A TRIANGLE RATIO. A ratio was the first thing tried
                // here and it picks badly at both ends: it left fir_sapling at 393k triangles (1.1x
                // off LOD 0, so nearly no saving on one of the heaviest meshes in the scene) while
                // being willing to reduce a 116-triangle moss. How coarse a level is says nothing
                // about how wrong it looks; Trifactor already measures that per level, in centimetres.
                //
                // The threshold is a shadow-map texel, near enough. A cascade covers its slice with
                // 2048 texels, so silhouette error below roughly that lands inside one texel and
                // cannot change the shadow -- geometry accurate to less than the thing sampling it is
                // detail nobody can see.
                {
                    constexpr f32 kShadowErrorCm = 20.0f;
                    u32 pick = 0;
                    for (u32 lvl = 1; lvl < ladder.handles.size(); ++lvl)
                        if (ladder.errorCm[lvl] <= kShadowErrorCm) pick = lvl;
                    if (pick > 0) {
                        // Keyed on every level's handle, not just LOD 0's: when --lod-select is on the
                        // lit pass submits whichever level it chose, and that handle must resolve too
                        // or the proxy silently stops applying to exactly the instances furthest away.
                        for (u32 lvl = 0; lvl < ladder.handles.size(); ++lvl)
                            if (ladder.triCounts[lvl] > ladder.triCounts[pick])
                                depthProxy_[ladder.handles[lvl]] = ladder.handles[pick];
                        AVER_INFO("[Mesh] '{}' depth proxy: LOD {} ({} tris, {:.1f}x less than LOD 0, "
                                  "{:.1f}cm error)",
                                  rel, pick, ladder.triCounts[pick],
                                  static_cast<f64>(ladder.triCounts.front()) /
                                      static_cast<f64>(ladder.triCounts[pick] ? ladder.triCounts[pick] : 1),
                                  ladder.errorCm[pick]);
                    }
                }
                meshLods_[id] = std::move(ladder);

                // Flat, all-levels-at-once cluster data for the per-cluster path (--lod-per-cluster).
                // `verts` is copied here (not moved) because the LOD-0 MeshHandle `h` above was
                // already created from it -- this copy is what a cache rebuild re-uploads later, the
                // exact upload the task brief says to measure.
                MeshClusterData cd;
                cd.verts = verts;
                trifactor::buildMeshClusterViews(md, cd.clusters, cd.clusterIndices);
                if (!cd.clusters.empty()) {
                    // Once per mesh, from data already resident -- see MeshClusterData::levelBounds'
                    // own comment. Never recomputed per frame or per instance.
                    trifactor::buildMeshClusterLevelBounds(cd.clusters, cd.levelBounds);
                    for (const trifactor::MeshClusterView& cv : cd.clusters)
                        cd.maxSphereRadius = std::max(cd.maxSphereRadius, cv.sphereRadius);
                    meshClusterData_[id] = std::move(cd);
                }

                // GPU cluster buffers for --lod-mesh-shader, built ONLY when the flag is on, so a run
                // that never asks for this feature never pays for the extra upload. Uses
                // buildMeshClusterGpuData (NOT buildMeshClusterViews' outIndices above): that one
                // keeps each meshlet's own local vertex/triangle block intact -- the shape the
                // amplification+mesh shader pair reads -- instead of pre-expanding every cluster's
                // triangles into global indices, which is the CPU-assembly cost this GPU path exists
                // to avoid paying at all, let alone per cut change.
                if (lodMeshShaderEnabled_) {
                    std::vector<trifactor::MeshClusterView> gpuBounds;
                    std::vector<trifactor::GpuMeshletDesc> gpuDesc;
                    std::vector<u32> gpuVerts, gpuTris;
                    trifactor::buildMeshClusterGpuData(md, gpuBounds, gpuDesc, gpuVerts, gpuTris);
                    if (!gpuBounds.empty()) {
                        if (rhi::IResourceFactory* res = e.device()->resources()) {
                            MeshClusterGpu gpu;
                            gpu.clusterCount = static_cast<u32>(gpuBounds.size());
                            auto upload = [&](const void* data, u64 bytes, const char* name) -> rhi::BufferHandle {
                                rhi::BufferDesc bd; bd.bytes = bytes; bd.kind = rhi::BufferKind::Upload;
                                bd.debugName = name;
                                const rhi::BufferHandle h = res->createBuffer(bd);
                                if (h) res->writeBuffer(h, data, bytes, 0);
                                return h;
                            };
                            gpu.bounds = upload(gpuBounds.data(), gpuBounds.size() * sizeof(trifactor::MeshClusterView), "lod-mesh-shader bounds");
                            gpu.desc   = upload(gpuDesc.data(),   gpuDesc.size()   * sizeof(trifactor::GpuMeshletDesc),  "lod-mesh-shader desc");
                            gpu.verts  = upload(gpuVerts.data(),  gpuVerts.size()  * sizeof(u32),                        "lod-mesh-shader verts");
                            gpu.tris   = upload(gpuTris.data(),   gpuTris.size()   * sizeof(u32),                        "lod-mesh-shader tris");
                            if (gpu.bounds && gpu.desc && gpu.verts && gpu.tris) {
                                rhi::BindingSetDesc bsd;
                                bsd.srvCount = 4;
                                bsd.srvKinds[0] = bsd.srvKinds[1] = bsd.srvKinds[2] = bsd.srvKinds[3] =
                                    rhi::SlotKind::StructuredBuffer;
#if AVER_MODULE_VOXI
                                // STAGE 3: this SAME set is table 0, so it also carries Voxi's merged
                                // GI/shadow slots at kClusterGiSrvBase.. -- see
                                // ensureLodMeshPipeline's register-map comment for the full layout.
                                // Declared here with EXACTLY giLayout()'s own kinds (VoxiRenderer.cpp)
                                // so this mesh's table 0 has the identical shape Voxi's own table 0
                                // has, just based four registers higher; the CONTENT (which texture,
                                // which buffer) is written later, not at mesh-load time -- see the
                                // per-frame sync next to lodMeshShaderEnabled_'s entity-loop check,
                                // which is what keeps this current across a live GI-quality or
                                // shadow-resolution change instead of freezing whatever was bound the
                                // moment this mesh's cluster data uploaded.
                                bsd.srvCount = kClusterGiSrvBase + voxi::kGiSrvCount;   // 4 + 9 = 13
                                bsd.srvKinds[kClusterGiSrvBase + 0] = rhi::SlotKind::Texture3D;           // t4 GI volume
                                bsd.srvKinds[kClusterGiSrvBase + 1] = rhi::SlotKind::Texture2D;           // t5 shadow map
                                bsd.srvKinds[kClusterGiSrvBase + 2] = rhi::SlotKind::AccelerationStructure; // t6 TLAS
                                bsd.srvKinds[kClusterGiSrvBase + 3] = rhi::SlotKind::StructuredBuffer;    // t7 RT verts
                                bsd.srvKinds[kClusterGiSrvBase + 4] = rhi::SlotKind::StructuredBuffer;    // t8 RT indices
                                bsd.srvKinds[kClusterGiSrvBase + 5] = rhi::SlotKind::StructuredBuffer;    // t9 RT instances
                                bsd.srvKinds[kClusterGiSrvBase + 6] = rhi::SlotKind::Texture2D;           // t10 RT shadow hist
                                bsd.srvKinds[kClusterGiSrvBase + 7] = rhi::SlotKind::Texture2D;           // t11 RT refl hist
                                bsd.srvKinds[kClusterGiSrvBase + 8] = rhi::SlotKind::Texture2D;           // t12 GI-only shadow
                                bsd.uavCount = voxi::kGiUavCount;
                                bsd.uavKinds[0] = rhi::SlotKind::Texture3D;   // u0 volume mip 0
                                bsd.uavKinds[1] = rhi::SlotKind::Texture3D;   // u1 injection accumulator
                                bsd.uavKinds[2] = rhi::SlotKind::Texture2D;   // u2 RT shadow hist (write)
                                bsd.uavKinds[3] = rhi::SlotKind::Texture2D;   // u3 RT refl hist (write)
#endif
                                gpu.bindingSet = res->createBindingSet(bsd);
                                if (gpu.bindingSet) {
                                    res->setSrvBuffer(gpu.bindingSet, 0, gpu.bounds, sizeof(trifactor::MeshClusterView), (u32)gpuBounds.size());
                                    res->setSrvBuffer(gpu.bindingSet, 1, gpu.desc,   sizeof(trifactor::GpuMeshletDesc),  (u32)gpuDesc.size());
                                    res->setSrvBuffer(gpu.bindingSet, 2, gpu.verts,  sizeof(u32), (u32)gpuVerts.size());
                                    res->setSrvBuffer(gpu.bindingSet, 3, gpu.tris,   sizeof(u32), (u32)gpuTris.size());
                                    // Voxi's slots (t4/t5, kClusterGiSrvBase..) are NOT populated
                                    // here -- see the per-frame sync's own comment on why binding
                                    // them once at upload time is the wrong lifetime for a texture
                                    // that can be resized or recreated at any later frame.
                                    meshClusterGpu_[id] = gpu;
                                } else {
                                    AVER_WARN("[LOD-MESH-SHADER] '{}' binding set failed; this mesh falls back to the CPU per-cluster path", rel);
                                }
                            } else {
                                AVER_WARN("[LOD-MESH-SHADER] '{}' GPU cluster buffer upload failed; this mesh falls back to the CPU per-cluster path", rel);
                            }
                        }
                    }
                }
            }
#endif
        }
        if (loaded || failed)
            AVER_INFO("[Mesh] {} project mesh(es) loaded from {}{}", loaded, dir,
                      failed ? (", " + std::to_string(failed) + " failed") : "");
#else
        (void)e;
#endif
    }

#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    // Creates the AS+MS+PS pipeline --lod-mesh-shader draws through. Tried EXACTLY ONCE per run
    // (lodMeshPipelineTried_ latches immediately, success or not) -- a failed compile or a tier-0
    // device means "the CPU per-cluster path runs instead", logged once, never retried every frame.
    // Safe to call every frame; only the first call (with the flag on) does real work.
    void ensureLodMeshPipeline(Engine& e) {
        if (lodMeshPipelineTried_) return;
        // THE FLAG IS TESTED BEFORE THE LATCH IS SET, which is the opposite of what it used to do.
        // Latching first means a single call made while the flag is still false burns the one
        // attempt this function will ever make, and "we already tried" then answers every later
        // call for the rest of the process. That is a booby trap for a function whose own comment
        // above says it is safe to call every frame: the ordering fix that now sets the flag early
        // makes this unreachable, and this makes a future reordering merely late instead of fatal.
        if (!lodMeshShaderEnabled_) return;
        lodMeshPipelineTried_ = true;

#if !AVER_MODULE_PBR
        // NO MATERIALS, NO CLUSTER PATH. Its pixel shader evaluates a pbr:: surface, and a build
        // without the material system has nothing for it to evaluate. Declining here is the honest
        // degrade rather than a silent one: the alternative -- shading from the per-object colour,
        // which is what this path did before it had materials -- is precisely the black-foliage bug
        // this change exists to fix, so shipping it as a fallback would reintroduce it on exactly
        // the configurations nobody looks at.
        AVER_INFO("[LOD-MESH-SHADER] built without the PBR module, so the GPU per-cluster path has "
                  "no materials to shade with; using the CPU paths instead");
        return;
#else
        const rhi::DeviceCaps caps = e.device()->caps();
        if (caps.meshShaderTier == 0 || caps.shaderModel < 65 || !caps.dxcAvailable) {
            AVER_WARN("[LOD-MESH-SHADER] this device (meshShaderTier={}, shaderModel={}, dxc={}) "
                      "cannot run the GPU per-cluster path -- house rule 6's degrade: falling back to "
                      "--lod-per-cluster/--lod-select, whichever else is on",
                      caps.meshShaderTier, caps.shaderModel, caps.dxcAvailable);
            return;
        }
        rhi::IResourceFactory* res = e.device()->resources();
        rhi::IRenderContext* ctx = e.device()->renderContext();
        if (!res || !ctx) {
            AVER_WARN("[LOD-MESH-SHADER] no resource factory / render context on this backend; falling back");
            return;
        }

        lodMeshLayout_ = rhi::PipelineLayout{};
        lodMeshLayout_.srvCount = 4;   // table 0, t0..t3: ClusterBounds, ClusterMeshletDesc, verts, tris
        // TABLE 1 IS THE MATERIAL, and adding it is what stops this path drawing black. The pixel
        // shader now evaluates a real surface, so it needs the material system's textures bound
        // somewhere -- and srvCount1 is the second table the RHI already offers, based immediately
        // above the first (at t4 without Voxi below, t13 with it).
        //
        // NO THIRD TABLE IS NEEDED -- STAGE 3'S WHOLE POINT. Full parity with the ordinary path
        // wants three sets at once: these cluster buffers, Voxi's GI volume and cascade atlas, and
        // the material textures. rhi::kBindingTableCount is 2 and STAYS 2 (see D3D12Device.cpp's
        // nullFill for why: the measured slot counts already fit table 0, and a third table would
        // also cost Vulkan a descriptor set it is not guaranteed to have four of). So instead of
        // opening one, Voxi's resources are MERGED into table 0 alongside the cluster buffers below;
        // table 1 stays the material's alone, exactly as it was before this stage.
#if AVER_MODULE_VOXI
        // STAGE 3'S REGISTER MAP, established here BEFORE any of it is code -- an off-by-one in this
        // comment would be a silent mis-sample, not a compile error, so this is what got checked
        // against giLayout() (VoxiRenderer.cpp) and VoxiGiShaders.hpp before anything below was
        // written:
        //   t0..t3   cluster geometry (above, unchanged) -- ClusterBounds/ClusterMeshletDesc/verts/tris
        //   t4       Voxi's GI volume  (kClusterGiSrvBase+0, Texture3D)  -- giShaderPrelude() reads it
        //   t5       Voxi's shadow map (kClusterGiSrvBase+1, Texture2D)  -- giShaderPrelude() reads it
        //   t6..t12  Voxi's TLAS / RT geometry table / RT history / GI-only shadow map -- RESERVED so
        //            this table-0 union has EXACTLY giLayout()'s shape (kGiSrvCount = 9), but never
        //            declared by giShaderPrelude(): this pipeline's pixel shader runs Voxi's non-ray-
        //            traced fallback only and reads none of them. See VoxiGiShaders.hpp's own comment
        //            on why over-provisioning here is deliberate, not an oversight.
        //   t13..t20 material (table 1, srvCount1 = pbr::kMaterialSrvCount, now based at t13)
        //   u0..u3   Voxi's volume-mip / accumulator / RT-history UAVs -- RESERVED, same reason as
        //            t6..t12: this pixel shader never writes any of them.
        //   s0       material sampler (kClusterMaterialSamplerSlot, unchanged)
        //   s1       Voxi's volume sampler (kClusterGiSamplerBase+0, linear-clamp)
        //   s2       Voxi's shadow sampler (kClusterGiSamplerBase+1, comparison-linear-clamp)
        //   b1       PerObject (unchanged)
        //   b3       VoxiFrame (kClusterGiFrameRegister) -- read by the pixel shader only
        //   b4       ClusterFrameCB (kFeatureFrameConstantRegister, unchanged) -- read by AS/MS only;
        //            see kClusterGiFrameRegister's own comment on why VoxiFrame could not share it
        lodMeshLayout_.srvCount += voxi::kGiSrvCount;   // t4..t12
        lodMeshLayout_.uavCount  = voxi::kGiUavCount;   // u0..u3, reserved; this PS never writes them
#endif
        lodMeshLayout_.srvCount1 = pbr::kMaterialSrvCount;
        lodMeshLayout_.samplers[kClusterMaterialSamplerSlot].filter        = rhi::Filter::Anisotropic;
        lodMeshLayout_.samplers[kClusterMaterialSamplerSlot].address       = rhi::AddressMode::Wrap;
        lodMeshLayout_.samplers[kClusterMaterialSamplerSlot].maxAnisotropy = 8;
        lodMeshLayout_.samplerCount = kClusterMaterialSamplerSlot + 1;
#if AVER_MODULE_VOXI
        // Voxi's own two samplers, at kClusterGiSamplerBase -- the SAME filter/address/compare
        // values giSamplers() (VoxiRenderer.cpp) gives its own s0/s1, just moved up because s0 here
        // is already the material's.
        lodMeshLayout_.samplers[kClusterGiSamplerBase + 0].filter  = rhi::Filter::Linear;
        lodMeshLayout_.samplers[kClusterGiSamplerBase + 0].address = rhi::AddressMode::Clamp;
        lodMeshLayout_.samplers[kClusterGiSamplerBase + 1].filter  = rhi::Filter::ComparisonLinear;
        lodMeshLayout_.samplers[kClusterGiSamplerBase + 1].address = rhi::AddressMode::Clamp;
        lodMeshLayout_.samplers[kClusterGiSamplerBase + 1].compare = rhi::CompareOp::LessEqual;
        lodMeshLayout_.samplerCount = kClusterGiSamplerBase + 2;   // 3: material(0), volume(1), shadow(2)
#endif
        // b1 (PerObject): the SAME 32-dword root-constants shape drawMesh() itself uses, so the
        // cluster shaders read real gWorld/gBaseColor/gMaterial. b4 (kFeatureFrameConstantRegister):
        // a root CBV, exactly the register the shared convention reserves for "a feature's own
        // per-frame/per-draw data" -- see RHIResources.hpp's own comment on it. Read by AS/MS
        // (ClusterFrameCB) only; the pixel shader below never touches it.
        lodMeshLayout_.constantDwords[rhi::kObjectConstantRegister] = rhi::kObjectConstantDwords;
        lodMeshLayout_.constantDwords[rhi::kFeatureFrameConstantRegister] = 0;
#if AVER_MODULE_VOXI
        // b3: VoxiFrame, a root CBV the pixel shader alone reads -- see kClusterGiFrameRegister's
        // own comment on why this cannot share ClusterFrameCB's b4.
        lodMeshLayout_.constantDwords[kClusterGiFrameRegister] = 0;
#endif

        // AS and MS come from the shared prelude alone: they touch no material state. The PIXEL
        // shader does not -- see below.
        const std::string defs = std::string("AVER_MS_CLUSTER=1;") + rhi::meshGeometryDefines(lodMeshLayout_);
        static const std::string kEmptySource;

        rhi::ShaderDesc asd;
        asd.prelude = rhi::sharedShaderPrelude();
        asd.source = kEmptySource.c_str();
        asd.entry = "ASMain";
        asd.stage = rhi::ShaderStage::Amplification;
        asd.minShaderModel = 65;
        asd.defines = defs.c_str();
        lodMeshAsShader_ = res->createShader(asd);

        rhi::ShaderDesc msd = asd;
        msd.entry = "MSClusterMain";
        msd.stage = rhi::ShaderStage::Mesh;
        lodMeshMsShader_ = res->createShader(msd);

        // THE PIXEL SHADER IS COMPOSED DIFFERENTLY FROM ITS AS/MS SIBLINGS, and that is the point of
        // this whole change. It is compiled as shared prelude + MATERIAL prelude (+ Voxi's GI/shadow
        // prelude, Stage 3, when AVER_MODULE_VOXI is compiled in) + its own source -- the order
        // PbrShaders.hpp documents, extended the same way Voxi's own pipeline extends it with its
        // own HLSL (see voxiShaderPrelude() in VoxiRenderer.cpp). Its predecessor lived inside the
        // shared prelude and therefore could not call averEvalMaterial at all -- the material
        // functions are not declared until the next prelude along.
        //
        // AVER_MS_CLUSTER is deliberately NOT defined for it: it needs neither the cluster buffers
        // nor the AS/MS entry points, only VSOut, which is unguarded.
#if AVER_MODULE_VOXI
        static const std::string kClusterPsPrelude =
            std::string(rhi::sharedShaderPrelude()) + pbr::materialShaderPrelude() + voxi::giShaderPrelude();
#else
        static const std::string kClusterPsPrelude =
            std::string(rhi::sharedShaderPrelude()) + pbr::materialShaderPrelude();
#endif
        // Based at THIS layout's own srvCount, so the material textures land in table 1 wherever
        // Stage 3's merge actually put it (t13 with Voxi compiled in, t4 without) -- the same call
        // Voxi makes against giLayout(), just with this layout's own number.
        // AVER_CLUSTER_PS_DEBUG=0 is the real shader; 1..7 isolate one input each when this path
        // renders wrong. See ClusterMaterialShader.hpp for what each one proved.
        std::string psDefs =
            pbr::materialShaderDefines(lodMeshLayout_.srvCount, kClusterMaterialSamplerSlot) +
            ";AVER_CLUSTER_PS_DEBUG=0";
#if AVER_MODULE_VOXI
        // AVER_CLUSTER_VOXI=1 is what switches PSClusterMain from the neutral sun.visibility=1.0 /
        // zero-indirect stand-in Stage 2 left it with to the real shadowFactor()/coneTracedIndirect()
        // calls -- see ClusterMaterialShader.hpp's own #if AVER_CLUSTER_VOXI ladder.
        psDefs += ";AVER_CLUSTER_VOXI=1;" +
                  voxi::giShaderDefines(kClusterGiSrvBase, kClusterGiSamplerBase, kClusterGiFrameRegister);
#endif
        static const std::string kClusterPsSource{sandbox::kClusterMaterialPS};

        rhi::ShaderDesc psd;
        psd.prelude = kClusterPsPrelude.c_str();
        psd.source  = kClusterPsSource.c_str();
        psd.entry = "PSClusterMain";
        psd.stage = rhi::ShaderStage::Pixel;
        psd.defines = psDefs.c_str();
        // SM 6.5 like its siblings. It has no SM6-only syntax of its own any more -- that was a
        // property of sharing a source string with ASMain's DispatchMesh -- but a pixel shader
        // paired with an AS/MS pipeline targeting a LOWER model is not something this tree has ever
        // done, and a release is not the place to find out whether it links.
        psd.minShaderModel = 65;
        lodMeshPsShader_ = res->createShader(psd);

        if (!lodMeshAsShader_ || !lodMeshMsShader_ || !lodMeshPsShader_) {
            AVER_WARN("[LOD-MESH-SHADER] AS/MS/PS compile failed; falling back to --lod-per-cluster/--lod-select");
            return;
        }

        rhi::GraphicsPipelineDesc pd;
        pd.as = lodMeshAsShader_;
        pd.ms = lodMeshMsShader_;
        pd.ps = lodMeshPsShader_;
        pd.layout = lodMeshLayout_;
        pd.cull = rhi::CullMode::None;
        pd.depth = {true, true, rhi::CompareOp::Less};
        pd.renderTargetCount = 1;
        pd.renderTargets[0] = e.device()->backbufferFormat();
        pd.depthFormat = e.device()->depthFormat();
        pd.sampleCount = e.device()->sampleCount();
        lodMeshPipeline_ = res->createGraphicsPipeline(pd);
        if (!lodMeshPipeline_) {
            AVER_WARN("[LOD-MESH-SHADER] pipeline creation failed; falling back to --lod-per-cluster/--lod-select");
            return;
        }
        lodMeshPipelineReady_ = true;
        AVER_INFO("[LOD-MESH-SHADER] GPU per-cluster pipeline ready (meshShaderTier={})", caps.meshShaderTier);
#endif   // AVER_MODULE_PBR: the no-materials build returned above
    }
#endif

    // Drops the project's meshes from the id table. The built-in primitives survive.
    void releaseProjectMeshes(Engine& e) {
        (void)e;   // only read under AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR, below
        // sceneMeshes_ is scene-only; with the module off loadProjectMeshes() never populated it (see
        // its own #if AVER_MODULE_SCENE above), so there is nothing here to erase from it either.
#if AVER_MODULE_SCENE
        for (const u64 id : projectMeshIds_) {
            sceneMeshes_.erase(id); meshTris_.erase(id);
#if AVER_MODULE_TRIFACTOR
            meshLods_.erase(id);
            meshClusterData_.erase(id);
#endif
        }
#if AVER_MODULE_TRIFACTOR
        // Every per-instance CPU-assembled cut handle is about to be invalid (its source mesh data
        // is gone) -- destroy each one explicitly rather than leaking GPU index/vertex buffers.
        for (auto& [ent, cache] : clusterCutCache_)
            if (cache.handle) e.device()->destroyMesh(cache.handle);
        clusterCutCache_.clear();
#endif
#endif
        projectMeshIds_.clear();
    }

    // Loads every .ocmat under Content/Materials and binds each to the surface token its stem interns to.
#if AVER_MODULE_PBR
    void loadProjectMaterials() {
#if AVER_MODULE_SCENE
        const std::string dir = project_.contentDir();
        if (dir.empty()) return;
        const std::string matDir = dir + "\\Materials";
        std::error_code ec;
        if (!std::filesystem::exists(matDir, ec)) return;

        u32 loaded = 0;
        for (std::filesystem::directory_iterator it(matDir, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            if (assetTypeFromPath(it->path().string()) != AssetType::Material) continue;
            const std::string stem = it->path().stem().string();
            const pbr::MaterialHandle h = materialForSurface(stem);
            if (!h) continue;
            surfaceMaterials_[aver_scene_material(0, stem.c_str())] = h;
            ++loaded;
        }
        if (loaded) AVER_INFO("[Material] {} project material(s) loaded from {}", loaded, matDir);
#endif
    }

    // Destroys every material the project owns. The texture cache behind them survives.
    void releaseProjectMaterials() {
        for (const auto& kv : materialAssets_) if (kv.second) pbr::MaterialLibrary::get().destroy(kv.second);
        materialAssets_.clear();
        surfaceMaterials_.clear();
    }
#endif

    // DECIDED 3 + slice 5: loads every .ocparticle under the project's content root into
    // particles::particleEffects(), keyed by fnv1a64(relative path) -- the SAME id space
    // contentIndex_/loadProjectMeshes already use for every other project asset, so a
    // CParticleEmitter::effect a level or a script names resolves the identical way a
    // CMeshRenderer::mesh or CAnimator::clip does. Recursive over the whole content root, matching
    // loadProjectMeshes rather than loadProjectMaterials' Content\Materials convention: DECIDED 3
    // gave .ocparticle no such folder rule.
    //
    // particles::particleEffects() is cleared first, matching anim::animSystem().clear()'s own reason
    // just above rebuildContentIndex: a stale id from a PREVIOUSLY open project must not keep
    // resolving once a different project supplies a different file at the same relative path. Safe
    // for --particle-test specifically because applyProject() (this function's only caller) runs
    // BEFORE the --particle-test block below registers its own hardcoded effects, not after -- see
    // that block's own comment for the ordering this relies on.
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    void loadProjectParticleEffects() {
        particles::particleEffects().clear();
        const std::string dir = project_.contentDir();
        if (dir.empty()) return;
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec)) return;

        u32 loaded = 0, failed = 0;
        for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            const std::string full = it->path().string();
            if (assetTypeFromPath(full) != AssetType::Particle) continue;

            std::string rel = std::filesystem::relative(it->path(), dir, ec).string();
            if (ec) continue;
            for (char& c : rel) if (c == '\\') c = '/';

            particles::ParticleEffect fx;
            std::string err;
            if (!fmt::loadOcparticle(full, fx, nullptr, &err)) {
                AVER_WARN("[Particles] {}", err);
                ++failed;
                continue;
            }
            particles::particleEffects().set(fnv1a64(std::string_view(rel)), fx);
            ++loaded;
        }
        if (loaded || failed)
            AVER_INFO("[Particles] {} project effect(s) loaded from {}{}", loaded, dir,
                      failed ? (", " + std::to_string(failed) + " failed") : "");
    }
#endif

    // Creates one actor's material and pins the actor's own metallic/roughness to the identity 1.
    void makeMaterialFor(MeshObj& o) {
#if AVER_MODULE_PBR
        pbr::MaterialDesc d;
        d.name            = o.name;
        d.metallicFactor  = o.metallic;
        d.roughnessFactor = o.roughness;
        o.material = pbr::MaterialLibrary::get().create(d);
        if (!o.material) { AVER_WARN("[Sandbox] no material for '{}'; it will draw with the fallback", o.name); return; }
        o.metallic = o.roughness = 1.0f;
#else
        (void)o;
#endif
    }

    // Submits the frame: the editor scene, the level world, gizmos, and the overlays.
    void onRender(Engine& e) override {
        handleManip(e);
        e.device()->setWireframe(wireframe_);
        // levelEntities_ exists only under AVER_MODULE_SCENE; with it off there is no loaded level to
        // hide the editor placeholders for, so the OR term is simply absent rather than always-false.
#if AVER_MODULE_SCENE
        hideEditorScene_ = playSessionActive() || !levelEntities_.empty();
#else
        hideEditorScene_ = playSessionActive();
#endif
        const bool hideEditorScene = hideEditorScene_;
        for (int i=0;i<(int)objects_.size();++i) {
            MeshObj& o = objects_[i];
            if (!o.visible || hideEditorScene) continue;
            Transform tr; tr.position=o.pos; tr.rotation=quatFromEulerDeg(o.rotDeg); tr.scale=o.scale;
            Mat4 w = tr.toMatrix();
            f32 col[4]={o.color[0],o.color[1],o.color[2],1};
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
            // setDrawBinding is sticky, so it is set unconditionally before every draw.
            if (pbr::MaterialSystem& ms = voxiRenderer_.materials(); ms.ready())
                e.device()->setDrawBinding(ms.bindingSet(o.material), &ms.constants(o.material),
                                           sizeof(pbr::MaterialConstants));
#endif
            e.device()->drawMesh(o.mesh, &w.m[0][0], col, o.metallic, o.roughness);
            if (i == sel_) selectionOutline_ = w, selectionMesh_ = o.mesh, hasSelection_ = true;
        }

#if AVER_MODULE_LANDSCAPE
        // The landscape pass: one direct select()+draw() call, the same hand-rolled shape as the
        // objects_ loop just above -- not an IRenderFeature (LandscapeRenderer implements none of its
        // virtuals) and not gated on hideEditorScene (terrain is real environment geometry, not an
        // editor placeholder, so it stays visible through Play just like the sky and fog do).
        if (landscapeLoaded_) updateLandscapeRingTiles(e.device(), eye_.x, eye_.y);

        if (landscapeLoaded_ && landscapeRenderer_) {
            // Bind the level's terrain material the first frame the material system is ready.
            // LEVEL LOAD CANNOT BE TRUSTED TO BE LATE ENOUGH: loadProjectMaterials() runs before
            // the landscape loads, but MaterialSystem::ready() also needs its GPU side up, and the
            // two do not have a guaranteed order. A bool test per frame is cheaper than reasoning
            // about that ordering, and it self-heals if the system comes up later.
            if (!landscapeMaterial_.empty() &&
                (!landscapeRenderer_->hasSurfaceBinding() || !landscapeUvTilingResolved_))
                applyLandscapeSurfaceToAll(e.device());

            landscape::SelectParams lp;
            lp.cameraCm[0] = eye_.x; lp.cameraCm[1] = eye_.y; lp.cameraCm[2] = eye_.z;
            // Same fovY (radians(60.0f)) and the same projScale formula trifactor::projScale uses,
            // over THIS frame's actual viewport height rather than SelectParams's own 540.0f default
            // -- a mismatched scale reads as terrain refining at the wrong distance, not as a crash,
            // which is exactly the kind of bug that would go unnoticed.
            lp.projScale = vpH_ / (2.0f * std::tan(radians(60.0f) * 0.5f));
            lp.useFrustum = true;
            f32 vpm[16];
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) vpm[r * 4 + c] = viewProj_.m[r][c];
            lp.frustum = landscape::Frustum::fromViewProj(vpm);
            // A SHARE of the renderer's one shared draw budget, not the whole thing -- see the member
            // block's own comment on kLandscapeMaxDrawsPerTile (docs/LANDSCAPE_EDITOR.md blocker 9).
            lp.maxDraws = kLandscapeMaxDrawsPerTile;

            landscape::SelectResult lsel;
            landscapeTree_.select(lp, lsel);

            // Sections carry their own world position in every sample (OcLandData::worldAt already
            // folds originCm in -- see ChunkMesh.cpp), so the transform LandscapeRenderer::draw()
            // applies on top is identity, not a placement matrix.
            static const f32 kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            landscapeRenderer_->draw(*e.device(), landscapeData_, landscapeTree_, lsel, kIdentity,
                                     landscapeUvTilingCm_);

            // The ring: every procedural tile currently resident around the camera, each its own
            // section drawn through the SAME select()+draw() pair, sharing the SAME per-tile budget --
            // that sharing is what keeps the total draws submitted across every resident section this
            // frame within the renderer's one real ceiling, no matter how many tiles are resident.
            for (auto& kv : landscapeRingTiles_) {
                LandscapeRingTile& tile = kv.second;
                if (!tile.renderer) continue;
                landscape::SelectResult rsel;
                tile.tree.select(lp, rsel);
                tile.renderer->draw(*e.device(), tile.data, tile.tree, rsel, kIdentity,
                                    landscapeUvTilingCm_);
            }
        }
#endif

#if AVER_MODULE_SCENE
        // Scene-entity pass: draws every live entity carrying a CMeshRenderer.
        {
            scene::World& w = scene::World::instance();
            int drawn = 0, culled = 0;
#if AVER_MODULE_TRIFACTOR
            lodStats_ = LodSelectStats{};   // this frame's counters, from zero -- see the struct comment
            lodClusterStats_ = LodClusterStats{};
            lodMeshShaderStats_ = LodMeshShaderStats{};
            ++lodClusterFrame_;
            // Sweep the per-instance cut cache: an entry an instance did not touch for a while (its
            // entity was destroyed, its CMeshRenderer removed, or lodPerClusterEnabled_ just got
            // turned off) leaks its GPU handle forever otherwise -- there is no destruction hook to
            // catch that here. 256 frames of grace before reclaiming, so a briefly-off-screen instance
            // (still ticking, still touching its cache entry every frame it's visible) is never
            // mistaken for a gone one; only checked every 64 frames, so the sweep itself is not a
            // per-frame cost.
            if (lodPerClusterEnabled_ && (lodClusterFrame_ % 64 == 0)) {
                for (auto it2 = clusterCutCache_.begin(); it2 != clusterCutCache_.end();) {
                    if (lodClusterFrame_ - it2->second.lastUsedFrame > 256) {
                        if (it2->second.handle) e.device()->destroyMesh(it2->second.handle);
                        it2 = clusterCutCache_.erase(it2);
                    } else {
                        ++it2;
                    }
                }
            }
#if AVER_MODULE_VOXI
            // STAGE 3: re-samples Voxi's CURRENT gVoxelTex_/shadowTex_ handles into every cluster
            // mesh's table-0 binding set BEFORE any entity is drawn through it this frame -- see
            // VoxiRenderer::bindGiResources' own comment and the register map in
            // ensureLodMeshPipeline. ONE PASS OVER meshClusterGpu_, not one call per entity: bounded
            // by the number of DISTINCT meshes carrying GPU cluster data, not by how many instances
            // of them are on screen, so it stays cheap with thousands of instances sharing a handful
            // of meshes. This is also what keeps the merged tables correct across a LIVE GI-quality
            // or shadow-resolution change: those recreate voxelTex_/shadowTex_ under fresh handles,
            // and a binding written once at mesh-upload time would never see the new ones.
            if (lodMeshShaderEnabled_ && lodMeshPipelineReady_) {
                if (rhi::IResourceFactory* giRes = e.device()->resources())
                    for (auto& kv : meshClusterGpu_)
                        if (kv.second.bindingSet)
                            voxiRenderer_.bindGiResources(*giRes, kv.second.bindingSet, kClusterGiSrvBase);
            }
#endif
#endif

            // The six frustum planes, from the camera's viewProj. ENGINE convention: row-vector, so
            // a clip coordinate is a dot with a COLUMN, and each plane is a sum or difference of two
            // columns. Derived per frame rather than cached: it is two dozen adds, and a stale
            // frustum culls things that are on screen.
            f32 pl[6][4];
            {
                const Mat4& m = viewProj_;
                for (int i = 0; i < 4; ++i) {
                    pl[0][i] = m.m[i][3] + m.m[i][0];   // left
                    pl[1][i] = m.m[i][3] - m.m[i][0];   // right
                    pl[2][i] = m.m[i][3] + m.m[i][1];   // bottom
                    pl[3][i] = m.m[i][3] - m.m[i][1];   // top
                    pl[4][i] = m.m[i][2];               // near
                    pl[5][i] = m.m[i][3] - m.m[i][2];   // far
                }
            }
#if AVER_MODULE_VOXI
            // ---- depth prepass phase: a SEPARATE, EARLIER, CONTIGUOUS walk over the SAME entities ----
            //
            // Bracketed in exactly ONE ScopedGpuStat, not one per draw: this engine's GPU stat tree
            // (D3D12Device.cpp's tsSlice_/kMaxGpuSpans) budgets 64 open spans a frame, and Electric
            // Dreams alone submits over a thousand instances -- one push/pop pair per depth-only draw
            // would blow that budget on the first few dozen entities and, worse, would not even measure
            // the right thing: a span's GPU time is everything between its two timestamps IN
            // SUBMISSION ORDER, so interleaving depth-only and colour draws (prepass_1, colour_1,
            // prepass_2, colour_2, ...) under per-draw markers would fold colour-pass time into the
            // "depth prepass" number. A single bracket around a CONTIGUOUS run of depth-only draws,
            // finished before the first colour draw starts, is what makes the span mean what it says.
            //
            // THIS IS ALSO WHY THE WALK RUNS TWICE rather than emitting a prepass draw inline per
            // instance inside the existing loop below: interleaving would have the identical timing
            // problem one level down even without markers -- "how much did the prepass cost" would be
            // inseparable from "how much did the colour draws in between cost" on the GPU timeline.
            //
            // EXCLUDED FROM THIS WALK, matching the task's own list: a SKINNED entity (its posed
            // vertex buffer is compute-written; see IDevice::meshVertexBuffer/drawMeshDepthPrepass's
            // own defensive re-check), the LANDSCAPE (drawn through LandscapeRenderer::draw(), a
            // wholly separate call site this loop never reaches at all -- see the AVER_MODULE_LANDSCAPE
            // block above), and the GPU CLUSTER MESH-SHADER PATH (dispatchMeshClusters draws its own
            // geometry from an amplification/mesh-shader pair with no depth-only twin here). The
            // CPU-per-cluster path (--lod-per-cluster) is ALSO excluded, for a narrower reason: its
            // cache (clusterCutCache_) is rebuilt-or-reused once per frame per entity, and running
            // that same decision twice (once here, once in the colour walk below) would either
            // duplicate the rebuild bookkeeping or read a cache the colour walk has not populated yet
            // depending on which walk runs first -- not unsafe, just not worth the complexity for a
            // path this task's own measurements never enable. The plain fallback path and the
            // discrete-LOD-level path (trifactor::chooseLevelCached, a pure function of camera and
            // entity -- see ClusterAdapt.cpp) both replicate safely here because neither has any
            // per-frame state a second call could disturb.
            if (e.device()->depthPrepassEnabled()) {
                if (rhi::IRenderContext* pctx = e.device()->renderContext()) {
                    rhi::ScopedGpuStat prepassScope(*pctx, "depth prepass");
                    const u32 pn = w.count();
                    for (u32 pi = 0; pi < pn; ++pi) {
                        const scene::Entity pent = w.at(pi);
                        if (w.destroyPending(pent)) continue;
                        const scene::CMeshRenderer* pmr =
                            w.component<scene::CMeshRenderer>(pent, scene::kComponentMeshRenderer);
                        if (!pmr || !(pmr->flags & scene::kMeshRendererVisible) || pmr->mesh == 0) continue;
                        const auto pit = sceneMeshes_.find(pmr->mesh);
                        if (pit == sceneMeshes_.end()) continue;
                        // Skinned: excluded (posed, compute-written buffer -- see the block comment).
                        if (skinnedScene_ && skinnedScene_->drawHandle(pent) != 0) continue;
#if AVER_MODULE_TRIFACTOR
                        // GPU cluster mesh-shader path: excluded (see the block comment).
                        if (lodMeshShaderEnabled_ && lodMeshPipelineReady_ && meshClusterGpu_.count(pmr->mesh)) continue;
                        // CPU per-cluster path: excluded (see the block comment).
                        if (lodPerClusterEnabled_ && meshClusterData_.count(pmr->mesh)) continue;
#endif
                        const Mat4& pwm = w.worldMatrix(pent);
                        // The SAME box-cull test the colour walk below runs -- mirrored rather than
                        // shared because the two walks' loop bodies are otherwise unrelated in shape,
                        // but this must never disagree with the colour walk about what is visible: a
                        // pixel this walk skips drawing depth for, that the colour walk goes on to
                        // draw with the prepassed (LessEqual/no-write) pipeline, would read whatever
                        // depth happened to already be there -- almost certainly wrong.
                        // DECLARED OUT HERE, NOT INSIDE THE CULL BLOCK, and that scope is the whole
                        // bug this once had. The world-space box was computed correctly for the
                        // frustum test and then went out of scope, so the LOD selection below
                        // re-derived a sphere from pmr->aabbMin/aabbMax -- which are LOCAL bounds
                        // (Components.hpp says so) -- and handed them to chooseLevelCached, whose own
                        // parameter is named worldSphereCenter and is compared against the world-space
                        // eye. The colour walk a few hundred lines down does it correctly from
                        // wlo/whi, so the two walks fed the SAME pure function two different inputs
                        // for the same instance and could pick DIFFERENT LOD levels for it.
                        //
                        // That is not a cosmetic mismatch: the prepass then writes depth for one mesh
                        // while the colour pass draws another, and since the prepassed colour PSO
                        // tests LessEqual with writes off, every colour fragment behind the wrong
                        // depth is silently dropped. It measured as fern clumps rendering visibly
                        // sparser -- 2.81% of pixels differing, falling to 0.04% (noise) with
                        // --no-lod-select, which is what isolated it.
                        bool poutside = false;
                        bool pHaveWorldBox = false;
                        Vec3 plo{1e30f, 1e30f, 1e30f}, phi{-1e30f, -1e30f, -1e30f};
                        {
                            const Vec3 lo{pmr->aabbMin[0], pmr->aabbMin[1], pmr->aabbMin[2]};
                            const Vec3 hi{pmr->aabbMax[0], pmr->aabbMax[1], pmr->aabbMax[2]};
                            if (hi.x > lo.x && hi.y > lo.y && hi.z > lo.z) {
                                pHaveWorldBox = true;
                                for (u32 c = 0; c < 8; ++c) {
                                    const Vec3 cp{(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z};
                                    const Vec3 t = xformPoint(pwm, cp);
                                    plo.x = std::fmin(plo.x, t.x); phi.x = std::fmax(phi.x, t.x);
                                    plo.y = std::fmin(plo.y, t.y); phi.y = std::fmax(phi.y, t.y);
                                    plo.z = std::fmin(plo.z, t.z); phi.z = std::fmax(phi.z, t.z);
                                }
                                for (u32 fi = 0; fi < 6 && !poutside; ++fi) {
                                    const f32 d = pl[fi][0] * (pl[fi][0] > 0 ? phi.x : plo.x)
                                                + pl[fi][1] * (pl[fi][1] > 0 ? phi.y : plo.y)
                                                + pl[fi][2] * (pl[fi][2] > 0 ? phi.z : plo.z)
                                                + pl[fi][3];
                                    if (d < 0.0f) poutside = true;
                                }
                            }
                        }
                        if (poutside) continue;

                        rhi::MeshHandle pmesh = pit->second;
#if AVER_MODULE_TRIFACTOR
                        // Discrete per-level LOD: replicated safely (pure function -- see the block
                        // comment). Same ladder lookup and same chooseLevelCached call the colour
                        // walk's own `else if (lodSelectEnabled_ ...)` branch makes below.
                        // GUARDED THE SAME WAY THE COLOUR WALK GUARDS ITS OWN, deliberately: that
                        // one reads `lodSelectEnabled_ && !skinned && haveWorldBox`, so this one must
                        // too, or an instance without a usable box takes the ladder here and LOD 0
                        // there -- the same divergence by a different route.
                        if (lodSelectEnabled_ && pHaveWorldBox) {
                            if (const auto plit = meshLods_.find(pmr->mesh); plit != meshLods_.end()) {
                                const MeshLodLadder& ladder = plit->second;
                                const Vec3 sphereCenter = (plo + phi) * 0.5f;
                                const f32 sphereRadius = dist(plo, phi) * 0.5f;   // world space, as
                                                                                  // chooseLevelCached
                                                                                  // requires
                                trifactor::View pview;
                                pview.eye = eye_;
                                pview.viewProj = viewProj_;
                                pview.viewportHeightPx = vpH_;
                                pview.verticalFovRadians = radians(60.0f);
                                const u32 plevel = trifactor::chooseLevelCached(
                                    ladder.errorCm, sphereCenter, sphereRadius, lodErrorThresholdPx_, pview);
                                pmesh = ladder.handles[plevel];
                            }
                        }
#endif
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
                        {
                            const i32 pmat = pmr->material;
                            u32 pauthored = 0;
                            if (const auto pit2 = surfaceMaterials_.find(pmat); pit2 != surfaceMaterials_.end())
                                pauthored = pit2->second;
                            if (pbr::MaterialSystem& pms = voxiRenderer_.materials(); pms.ready())
                                e.device()->setDrawBinding(pms.bindingSet(pauthored), &pms.constants(pauthored),
                                                           sizeof(pbr::MaterialConstants));
                        }
#endif
                        e.device()->drawMeshDepthPrepass(pmesh, &pwm.m[0][0]);
                    }
                }
            }
#endif // AVER_MODULE_VOXI

            // IS THE FRAME CPU-BOUND OR GPU-BOUND? Establishing that took a dozen capture runs and
            // three wrong guesses, because --frame-time reports WHOLE frames from the CPU and a CPU
            // number that includes waiting for the GPU looks exactly like CPU work. Two timers
            // answer it directly and cost two clock reads a frame: this one, and the streamer's
            // below. The answer on Electric Dreams is 8.2ms of walk and 0.7ms of streaming inside a
            // 76ms frame -- so it is GPU-bound, and nothing done to this loop can matter.
            const auto tWalk0 = std::chrono::steady_clock::now();
            f64 dispatchMs = 0.0;

            const u32 n = w.count();

#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
            // ---- occlusion phase 0: collect every entity's world AABB, and split the walk order ----
            //
            // A DEDICATED PRE-WALK, not the main loop's own per-entity box computation, because
            // occluder_->testBatch() below is ONE GPU dispatch over every candidate at once (see
            // Occlusion.hpp's own comment on why: a synchronous readback per entity would be 6,370
            // waitIdle() calls instead of one) and it needs the WHOLE list -- including pass-1
            // entities, which the main loop reaches only AFTER they are already drawn -- before the
            // main loop has visited any of them. Degenerate-box entities (no valid AABB) are left out
            // here exactly as the main loop's own frustum cull leaves them un-culled below: "must not
            // vanish" applies to occlusion the same way it applies to the frustum.
            //
            // pass1Count_ entities -- occlusionWasVisible(ent) true, or never tested -- sort FIRST in
            // occlusionOrder_ and are drawn UNCONDITIONALLY, exactly as every entity is when this flag
            // is off; the rest sort after and are gated on this frame's testBatch() result once the
            // pyramid built from the first group exists. See this file's occlusion module README and
            // Occlusion.hpp's top comment for the two-pass argument this order encodes.
            u32 occlusionPass1Count = n;
            bool occlusionPyramidBuilt = false;
            if (occlusionCullEnabled_ && occluder_) {
                occlusionBoxes_.clear();
                occlusionBoxEntities_.clear();
                occlusionOrder_.resize(n);
                u32 head = 0, tail = n;   // pass-1 fills from the front, pass-2 from the back
                for (u32 k = 0; k < n; ++k) {
                    const scene::Entity e2 = w.at(k);
                    (occlusionWasVisible(e2) ? occlusionOrder_[head++] : occlusionOrder_[--tail]) = k;

                    if (w.destroyPending(e2)) continue;
                    const scene::CMeshRenderer* mr2 =
                        w.component<scene::CMeshRenderer>(e2, scene::kComponentMeshRenderer);
                    if (!mr2 || !(mr2->flags & scene::kMeshRendererVisible) || mr2->mesh == 0) continue;
                    // The SAME static-mesh-bounds override the main loop below applies (search "A
                    // STATIC entity gets its bounds from the asset") -- MUST run here too, or this
                    // pre-walk's box disagrees with the one the main loop uses for frustum culling
                    // the very first frame an entity is visited, before either pass has ever written
                    // corrected bounds into its CMeshRenderer.
                    const bool skinned2 = skinnedScene_ && skinnedScene_->drawHandle(e2) != 0;
                    if (!skinned2)
                        if (const auto bit2 = meshBounds_.find(mr2->mesh); bit2 != meshBounds_.end()) {
                            auto* mw2 = const_cast<scene::CMeshRenderer*>(mr2);
                            mw2->aabbMin[0] = bit2->second.first.x;  mw2->aabbMin[1] = bit2->second.first.y;
                            mw2->aabbMin[2] = bit2->second.first.z;
                            mw2->aabbMax[0] = bit2->second.second.x; mw2->aabbMax[1] = bit2->second.second.y;
                            mw2->aabbMax[2] = bit2->second.second.z;
                        }
                    const Vec3 lo2{mr2->aabbMin[0], mr2->aabbMin[1], mr2->aabbMin[2]};
                    const Vec3 hi2{mr2->aabbMax[0], mr2->aabbMax[1], mr2->aabbMax[2]};
                    if (!(hi2.x > lo2.x && hi2.y > lo2.y && hi2.z > lo2.z)) continue;   // degenerate
                    const Mat4& wm2 = w.worldMatrix(e2);
                    Vec3 wlo2{1e30f, 1e30f, 1e30f}, whi2{-1e30f, -1e30f, -1e30f};
                    for (u32 c = 0; c < 8; ++c) {
                        const Vec3 p{(c & 1) ? hi2.x : lo2.x, (c & 2) ? hi2.y : lo2.y, (c & 4) ? hi2.z : lo2.z};
                        const Vec3 t = xformPoint(wm2, p);
                        wlo2.x = std::fmin(wlo2.x, t.x); whi2.x = std::fmax(whi2.x, t.x);
                        wlo2.y = std::fmin(wlo2.y, t.y); whi2.y = std::fmax(whi2.y, t.y);
                        wlo2.z = std::fmin(wlo2.z, t.z); whi2.z = std::fmax(whi2.z, t.z);
                    }
                    aver::occlusion::Aabb box;
                    box.min[0] = wlo2.x; box.min[1] = wlo2.y; box.min[2] = wlo2.z;
                    box.max[0] = whi2.x; box.max[1] = whi2.y; box.max[2] = whi2.z;
                    occlusionBoxes_.push_back(box);
                    occlusionBoxEntities_.push_back(e2);
                }
                // `head` is now exactly the pass-1 count: every index landed on one side or the
                // other, head counting up from the front and tail down from the back, so head == tail
                // once the loop above has placed all n.
                occlusionPass1Count = head;
                if (rhi::IResourceFactory* occRes = e.device()->resources()) {
                    rhi::TextureDesc sceneDesc;
                    // sceneDepthTexture() itself supplies the size (via textureInfo below): asking
                    // occluder_ to size its pyramid off anything else risks it disagreeing with the
                    // ACTUAL depth buffer the seed pass is about to read.
                    if (const rhi::TextureHandle depthTex = e.device()->sceneDepthTexture();
                        depthTex && occRes->textureInfo(depthTex, sceneDesc)) {
                        occluder_->ensureSized(*occRes, sceneDesc.width, sceneDesc.height, e.device()->sampleCount());
                    }
                }
            } else {
                occlusionOrder_.clear();   // empty means "no reordering" -- see the loop below
            }

            // Runs buildPyramid()+testBatch() ONCE this frame -- either at the pass-1/pass-2 boundary
            // inside the main loop below (the common case, so pass-2 entities can be gated THIS
            // frame), or here as a fallback when pass-2 is empty (occlusionPass1Count == n: nothing
            // to gate this frame, but occlusionVisible_ still has to be refreshed for every entity or
            // the split above never budges from "everyone is pass-1" and this frame's demotions --
            // an entity that just became occluded -- would never be discovered).
            const auto occlusionBuildAndTest = [&]() {
                if (occlusionPyramidBuilt) return;
                occlusionPyramidBuilt = true;
                if (occlusionBoxes_.empty()) return;
                rhi::IRenderContext* pctx = e.device()->renderContext();
                rhi::IResourceFactory* occRes = e.device()->resources();
                const rhi::TextureHandle depthTex = e.device()->sceneDepthTexture();
                if (!pctx || !occRes || !depthTex) return;
                occluder_->buildPyramid(*pctx, depthTex, &viewProj_.m[0][0]);
                occluder_->testBatch(*pctx, *occRes, occlusionBoxes_.data(),
                                     static_cast<u32>(occlusionBoxes_.size()), occlusionResults_);
                u32 c = 0, t = 0;
                occluder_->lastTestCounts(c, t);
                occlusionCulledAccum_ += c;
                occlusionTestedAccum_ += t;
                ++occlusionReportFrames_;
                for (usize bi = 0; bi < occlusionBoxEntities_.size(); ++bi)
                    occlusionVisible_[occlusionBoxEntities_[bi]] = occlusionResults_[bi] != 0;
            };
#endif

            for (u32 oi = 0; oi < n; ++oi) {
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
                const u32 i = occlusionOrder_.empty() ? oi : occlusionOrder_[oi];
                if (occlusionCullEnabled_ && occluder_ && oi == occlusionPass1Count) occlusionBuildAndTest();
#else
                const u32 i = oi;
#endif
                const scene::Entity ent = w.at(i);
                if (w.destroyPending(ent)) continue;
                const scene::CMeshRenderer* mr =
                    w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
                if (!mr || !(mr->flags & scene::kMeshRendererVisible) || mr->mesh == 0) continue;
                const auto it = sceneMeshes_.find(mr->mesh);
                if (it == sceneMeshes_.end()) continue;
                const Mat4& wm = w.worldMatrix(ent);

                // A STATIC entity gets its bounds from the asset. A skinned one already had them
                // written this frame by SkinnedScene from its ACTUAL POSE, so leave those alone --
                // overwriting with the rest box is exactly the popping this exists to stop.
                const bool skinned = skinnedScene_ && skinnedScene_->drawHandle(ent) != 0;
                if (!skinned)
                    if (const auto bit = meshBounds_.find(mr->mesh); bit != meshBounds_.end()) {
                        auto* mw = const_cast<scene::CMeshRenderer*>(mr);
                        mw->aabbMin[0] = bit->second.first.x;  mw->aabbMin[1] = bit->second.first.y;
                        mw->aabbMin[2] = bit->second.first.z;
                        mw->aabbMax[0] = bit->second.second.x; mw->aabbMax[1] = bit->second.second.y;
                        mw->aabbMax[2] = bit->second.second.z;
                    }

                // Frustum cull on the world-space extent of the entity's own box. A DEGENERATE box
                // is drawn rather than culled: an entity whose bounds were never filled in must not
                // vanish, and being conservative costs a draw call where being wrong costs a
                // character.
                bool haveWorldBox = false;
                Vec3 wlo{1e30f, 1e30f, 1e30f}, whi{-1e30f, -1e30f, -1e30f};
                {
                    const Vec3 lo{mr->aabbMin[0], mr->aabbMin[1], mr->aabbMin[2]};
                    const Vec3 hi{mr->aabbMax[0], mr->aabbMax[1], mr->aabbMax[2]};
                    if (hi.x > lo.x && hi.y > lo.y && hi.z > lo.z) {
                        for (u32 c = 0; c < 8; ++c) {
                            const Vec3 p{(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z};
                            const Vec3 t = xformPoint(wm, p);
                            wlo.x = std::fmin(wlo.x, t.x); whi.x = std::fmax(whi.x, t.x);
                            wlo.y = std::fmin(wlo.y, t.y); whi.y = std::fmax(whi.y, t.y);
                            wlo.z = std::fmin(wlo.z, t.z); whi.z = std::fmax(whi.z, t.z);
                        }
                        haveWorldBox = true;
                        bool outside = false;
                        for (u32 pi = 0; pi < 6 && !outside; ++pi) {
                            // The corner FURTHEST along the plane normal. If even that one is behind,
                            // every corner is, and only then is the box definitely out.
                            const f32 d = pl[pi][0] * (pl[pi][0] > 0 ? whi.x : wlo.x)
                                        + pl[pi][1] * (pl[pi][1] > 0 ? whi.y : wlo.y)
                                        + pl[pi][2] * (pl[pi][2] > 0 ? whi.z : wlo.z)
                                        + pl[pi][3];
                            if (d < 0.0f) outside = true;
                        }
                        if (outside) { ++culled; continue; }
                    }
                }
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
                // PASS-2 ONLY: `oi < occlusionPass1Count` entities were drawn unconditionally, before
                // occlusionBuildAndTest() ever ran -- see the loop header's own comment. Everything
                // from here down is an entity occlusionBuildAndTest() (triggered right when `oi`
                // crossed occlusionPass1Count, above) has ALREADY tested against a pyramid built from
                // every one of those pass-1 draws, so occlusionWasVisible(ent) is this frame's own
                // fresh answer, not a stale one -- exactly the same-frame guarantee the module's own
                // top comment argues for. A box excluded from occlusionBoxes_ (degenerate — see the
                // pre-walk) was never tested and occlusionWasVisible defaults such an entity to
                // visible, so it reaches here and draws, same as haveWorldBox==false already does for
                // frustum culling two paragraphs up.
                if (occlusionCullEnabled_ && occluder_ && oi >= occlusionPass1Count && haveWorldBox &&
                    !occlusionWasVisible(ent)) {
                    continue;
                }
#endif
                const i32 mat = mr->material;
                f32 col[4] = {0.80f, 0.80f, 0.85f, 1.0f};
                f32 metallic = 0.0f, roughness = 0.5f;

                u32 authored = 0;
#if AVER_MODULE_PBR
                if (const auto it2 = surfaceMaterials_.find(mat); it2 != surfaceMaterials_.end())
                    authored = it2->second;
#endif
                if (authored) {
                    col[0] = col[1] = col[2] = 1.0f;
                    metallic = roughness = 1.0f;
                } else if (const auto look = surfaceLooks_.find(mat); look != surfaceLooks_.end()) {
                    col[0] = look->second.col[0]; col[1] = look->second.col[1]; col[2] = look->second.col[2];
                    metallic = look->second.metallic; roughness = look->second.roughness;
                }
                // This entity's material, resolved ONCE and kept in locals because TWO paths below
                // need it and they consume it through different objects: the ordinary drawMesh()
                // reads the DEVICE's copy, the cluster dispatch has to be handed the CONTEXT's (see
                // its own comment). Declared outside the module guard, like `mesh` and
                // `clusterDispatched`, so the code below compiles with PBR or VOXI switched off --
                // it just stays zero, which means "no per-draw material", exactly as before.
                rhi::BindingSetHandle matSet = 0;
                const void* matConstants = nullptr;
                u32 matConstantBytes = 0;
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
                if (pbr::MaterialSystem& ms = voxiRenderer_.materials(); ms.ready()) {
                    matSet = ms.bindingSet(authored);
                    matConstants = &ms.constants(authored);   // a reference into the system's own storage
                    matConstantBytes = sizeof(pbr::MaterialConstants);
                    e.device()->setDrawBinding(matSet, matConstants, matConstantBytes);
                }
#endif
                // The scene test paints its two entities so a probe can tell which it is looking
                // at. Only ever active behind --skin-scene-test.
                if (skinScene_) {
                    if (ent == static_cast<scene::Entity>(skinScene_->subjectEntity()))
                        aver::editor::SkinSceneTest::subjectColor(col);
                    else if (ent == static_cast<scene::Entity>(skinScene_->referenceEntity()))
                        aver::editor::SkinSceneTest::referenceColor(col);
                }

                // THE SEAM, and it is one line because the design made it one. A skinned entity's
                // posed vertices live in a DIFFERENT MeshHandle sharing this one's index buffer, so
                // substituting the handle reaches every pass at once. Zero means "not skinned",
                // never "not drawn" -- a character that fails to skin must still appear.
                rhi::MeshHandle mesh = it->second;
                // Set true only by the GPU per-cluster path below (AVER_MODULE_TRIFACTOR only) when it
                // actually dispatches this instance's geometry itself -- declared unconditionally,
                // like `mesh` just above, so the plain drawMesh() call at the end of this block can
                // check it regardless of whether that module is compiled in.
                bool clusterDispatched = false;
#if AVER_MODULE_TRIFACTOR
                // Virtualized-geometry LOD selection (aver::trifactor::ClusterAdapt/ClusterSelect).
                // Per-LEVEL, not per-cluster -- see ClusterAdapt.hpp's file header for why. Skipped
                // for a skinned entity (its posed handle below always wins anyway, per THE SEAM
                // comment) and for a mesh with no LOD ladder (single-LOD meshes are unaffected, same
                // as before this feature existed). NO CACHE: chooseLevelCached is a handful of float
                // ops per instance and touches no GPU resource, so it is cheap enough to run fresh
                // EVERY frame -- see the task report for why that trivially satisfies "a moving
                // camera must still update the choice" without needing an invalidation rule at all.
                // GPU per-cluster path (--lod-mesh-shader) wins over EVERYTHING below when the mesh
                // has GPU cluster buffers AND the pipeline came up on this device -- it dispatches the
                // geometry itself (DispatchMesh, inside dispatchMeshClusters), so `mesh` is never
                // substituted and the ordinary drawMesh() call at the end of this block is skipped
                // entirely for this instance. Falls through to the CPU per-cluster / per-level paths
                // unchanged whenever it does not apply (skinned, no world box, no GPU cluster data for
                // this mesh, or the pipeline never came up on this device/run) -- see
                // ensureLodMeshPipeline's own comment for the degrade.
                if (lodMeshShaderEnabled_ && lodMeshPipelineReady_ && !skinned && haveWorldBox) {
                    if (const auto git = meshClusterGpu_.find(mr->mesh); git != meshClusterGpu_.end()) {
                        const MeshClusterGpu& gpu = git->second;
                        if (rhi::IRenderContext* ctx = e.device()->renderContext(); ctx && gpu.clusterCount) {
                            trifactor::View view;
                            view.eye = eye_;
                            view.viewProj = viewProj_;
                            view.viewportHeightPx = vpH_;
                            view.verticalFovRadians = radians(60.0f);
                            const f32 worldScale = xformVec(wm, Vec3{1, 0, 0}).size();

                            // ClusterFrameCB (b4): budget CLAMPED above zero here, on the CPU, before
                            // upload -- ASMain does not re-clamp (see its own comment) -- and the six
                            // frustum planes copied verbatim from the SAME Frustum::fromViewProj the
                            // CPU reference itself calls, so the GPU test runs against the identical
                            // numbers, not a second derivation of them.
                            ClusterFrameCB frameCb;
                            frameCb.budgetPx = std::max(lodErrorThresholdPx_, trifactor::kMinClusterBudgetPx);
                            frameCb.projScale = trifactor::projScale(view);
                            frameCb.worldScale = worldScale;
                            const trifactor::Frustum frustum = trifactor::Frustum::fromViewProj(view.viewProj);
                            static_assert(sizeof(frameCb.planes) == sizeof(frustum.plane),
                                          "ClusterFrameCB::planes must match trifactor::Frustum::plane byte for byte");
                            std::memcpy(frameCb.planes, frustum.plane, sizeof(frameCb.planes));

                            // PerObject (b1): the SAME 32-dword layout drawMesh() itself packs
                            // (world, base colour, metallic/roughness, then the frozen shading-model
                            // tail), so PSClusterMain's plainShadeSurface reads real, live values.
                            f32 consts[rhi::kObjectConstantDwords];
                            std::memcpy(consts, &wm.m[0][0], 16 * sizeof(f32));
                            std::memcpy(consts + 16, col, 4 * sizeof(f32));
                            consts[20] = metallic; consts[21] = roughness; consts[22] = 0.0f; consts[23] = 0.0f;
                            const u32 shadingModel = 0;   // AVER_MODEL_STANDARD
                            std::memcpy(consts + 24, &shadingModel, sizeof(shadingModel));
                            consts[25] = 0.04f; consts[26] = 1.0f; consts[27] = 0.0f;
                            consts[28] = consts[29] = consts[30] = consts[31] = 0.0f;

                            const auto tDis0 = std::chrono::steady_clock::now();
                            ctx->setPipeline(lodMeshPipeline_);
                            ctx->setBindingSet(gpu.bindingSet, 0);
                            // THE MATERIAL, ON THE CONTEXT -- and this line is why the foliage on
                            // this path drew black. device()->setDrawBinding above records the
                            // material on the DEVICE, and the device only forwards it to the context
                            // from inside drawMesh(), which this path deliberately never calls. So
                            // the table-1 binding and the b2 block that dispatchMeshClusters applies
                            // were whatever some EARLIER draw happened to leave sticky on the
                            // context: another entity's textures, or Voxi's fallback set, whose
                            // metal-rough map is white. metallic = gMaterial.x * gMetallicFactor *
                            // map.metalRough.y then came out 1, kdAlbedo = (1 - metallic) * albedo
                            // came out 0, and the whole diffuse lobe vanished -- which reads as
                            // black even though the textures, the b2 constants and the albedo all
                            // measured correct, because they were a DIFFERENT material's and
                            // happened to look plausible.
                            if (matSet) ctx->setDrawBinding(matSet, matConstants, matConstantBytes);
                            ctx->setConstants(rhi::kObjectConstantRegister, consts, rhi::kObjectConstantDwords);
                            ctx->setConstantBuffer(rhi::kFeatureFrameConstantRegister, &frameCb, sizeof(frameCb));
#if AVER_MODULE_VOXI
                            // b3: VoxiFrame, the SAME bytes Voxi's own scenePass binds at b4 for the
                            // ordinary path this frame -- see kClusterGiFrameRegister's own comment
                            // on why this path cannot reuse b4 itself. Bound every draw rather than
                            // once per mesh (like the Voxi resource sync above): a root CBV pointer
                            // set is cheap, and this keeps AVER_CLUSTER_VOXI's shadowFactor()/
                            // coneTracedIndirect() reading this frame's cascades even if Voxi has not
                            // finished init() yet (giFrameConstants() still returns a valid, if all-
                            // zero, block in that case -- shadowFactor/coneTracedIndirect degrade the
                            // same way Voxi's own gShadowParams.y/gVoxelParams.w checks do).
                            ctx->setConstantBuffer(kClusterGiFrameRegister, voxiRenderer_.giFrameConstants(),
                                                   voxiRenderer_.giFrameConstantBytes());
#endif
                            ctx->dispatchMeshClusters(mesh, gpu.clusterCount);
                            dispatchMs += std::chrono::duration<f64, std::milli>(
                                std::chrono::steady_clock::now() - tDis0).count();
                            clusterDispatched = true;

#if AVER_MODULE_VOXI
                            // DEFECT 2's FIX, in full: this instance's LIT pixels already came from
                            // dispatchMeshClusters above, so the ordinary IDevice::drawMesh() call at
                            // the bottom of this loop is skipped for it (see `if (!clusterDispatched)`
                            // there) -- and IRenderFeature::submitDraw is called from EXACTLY ONE place
                            // in the engine, D3D12Device::drawMesh/VulkanDevice::drawMesh (see this
                            // file's own [Sandbox] warning a few hundred lines up). Skipping drawMesh()
                            // therefore also skips submitDraw(), which is the ONLY way geometry reaches
                            // VoxiRenderer::draws_ -- so this instance never appeared in shadowPass,
                            // giShadowPass, or voxelizePass. A tree that casts no shadow is not a
                            // cheaper tree, it is a wrong one.
                            //
                            // voxiRenderer_.submit() is VoxiRenderer's own public, non-virtual method
                            // (submitDraw() is only the IRenderFeature override forwarding to it) and
                            // does nothing but append one Draw to this frame's list -- no GPU commands,
                            // no rhiContext_ access, so it is safe to call here regardless of what
                            // dispatchMeshClusters just bound on `ctx`. Passing `mesh` (this instance's
                            // FULL-detail handle -- the same one drawMesh() would have used had this
                            // instance not been cluster-dispatched) rather than a depth proxy directly
                            // is deliberate: submit() already resolves d.depthMesh through the SAME
                            // depthProxyFn_ every ordinary instance of this mesh goes through (see
                            // VoxiRenderer.cpp's submit(), and SandboxApp's own depthProxy_ population
                            // at mesh-load time, keyed by this exact handle) -- so the depth-only passes
                            // draw the cheap proxy exactly as they already do for this mesh's discrete
                            // instances, with no new resolution logic needed here. `mesh`, `col`,
                            // `metallic`, `roughness`, `matSet`, `matConstants`, `matConstantBytes` are
                            // all already resolved above (see this block's own comment on why the
                            // material triple is computed once, outside either draw path).
                            voxiRenderer_.submit(mesh, &wm.m[0][0], col, metallic, roughness, matSet,
                                                  matConstants, matConstantBytes);
#endif

                            ++lodMeshShaderStats_.instancesTested;
                            lodMeshShaderStats_.clustersDispatched += gpu.clusterCount;

                            // Informational counters -- see LodMeshShaderStats' own comment: the SAME
                            // CPU reference (trifactor::selectClusterCut), over the SAME world-space
                            // clusters and budget the GPU dispatch just used, sampled every 64 frames
                            // rather than every frame, per the task's own "sample it" allowance.
                            //
                            // GATED ON lodClusterStatsEnabled_ (--lod-cluster-stats), OFF BY DEFAULT --
                            // the SAME flag and the SAME reasoning as the per-level path's own
                            // informational cluster telemetry (see that flag's own comment: "a real,
                            // extra... CPU cost... must not be conflated when reporting numbers"). THE
                            // SAMPLE ITSELF USED TO BE A HITCH, even after THE INSTANCE-LEVEL SHORTCUT
                            // below: measured directly, the handful of instances the shortcut cannot
                            // prove (genuinely mixed-level, typically the closest/largest-DAG trees) are
                            // still expensive enough on their own that this block cost ~1.1 SECONDS on
                            // this sampled frame even with the shortcut applied to everything else --
                            // and because 64 divides --frames 128 evenly, that sampled frame is
                            // GUARANTEED to be the last frame of exactly this task's own benchmark
                            // command, not a rare coincidence. A once-per-second-ish stall is not an
                            // acceptable cost for a log line no render pass reads, so it no longer runs
                            // unless a caller explicitly asked for the counters it produces.
                            if (lodClusterStatsEnabled_ && lodClusterFrame_ % 64 == 0) {
                                if (const auto cit2 = meshClusterData_.find(mr->mesh); cit2 != meshClusterData_.end()) {
                                    const MeshClusterData& mcd = cit2->second;
                                    bool sampledShortcut = false;
                                    if (const auto lit2 = meshLods_.find(mr->mesh); lit2 != meshLods_.end()) {
                                        const MeshLodLadder& ladder2 = lit2->second;
                                        const Vec3 sphereCenter2 = (wlo + whi) * 0.5f;
                                        const f32 sphereRadius2 = dist(wlo, whi) * 0.5f;
                                        const u32 candidateLevel2 = trifactor::chooseLevelCached(
                                            ladder2.errorCm, sphereCenter2, sphereRadius2,
                                            lodErrorThresholdPx_, view);
                                        if (candidateLevel2 < mcd.levelBounds.size() &&
                                            trifactor::provablySingleLevelCut(
                                                mcd.levelBounds, candidateLevel2, sphereCenter2, sphereRadius2,
                                                mcd.maxSphereRadius * worldScale, lodErrorThresholdPx_, view)) {
                                            lodMeshShaderStats_.survivors += mcd.levelBounds[candidateLevel2].count;
                                            lodMeshShaderStats_.trianglesDrawn +=
                                                mcd.levelBounds[candidateLevel2].triangleCount;
                                            lodMeshShaderStats_.maxDistinctLevelsSeen =
                                                std::max(lodMeshShaderStats_.maxDistinctLevelsSeen, 1u);
                                            ++lodMeshShaderStats_.instancesShortcut;
                                            sampledShortcut = true;
                                        }
                                    }
                                    if (!sampledShortcut) {
                                        std::vector<trifactor::MeshClusterView> worldClusters = mcd.clusters;
                                        for (trifactor::MeshClusterView& cv : worldClusters) {
                                            cv.sphereCenter = xformPoint(wm, cv.sphereCenter);
                                            cv.sphereRadius *= worldScale;
                                            cv.coneApex = xformPoint(wm, cv.coneApex);
                                            cv.coneAxis = xformVec(wm, cv.coneAxis).getSafeNormal();
                                        }
                                        const trifactor::ClusterCutResult cr = trifactor::selectClusterCut(
                                            worldClusters, lodErrorThresholdPx_, view, true);
                                        lodMeshShaderStats_.survivors += cr.stats.drawn;
                                        lodMeshShaderStats_.trianglesDrawn += cr.stats.trianglesAfter;
                                        if (cr.stats.distinctLevels > 1) ++lodMeshShaderStats_.instancesMixedLevels;
                                        lodMeshShaderStats_.maxDistinctLevelsSeen =
                                            std::max(lodMeshShaderStats_.maxDistinctLevelsSeen, cr.stats.distinctLevels);
                                    }
                                }
                            }
                        }
                    }
                }
                // PER-CLUSTER path wins over per-LEVEL when both are enabled -- see setLodPerCluster's
                // own comment. This is what actually mixes LOD levels within one instance's draw; the
                // per-level `else if` below it is entirely unchanged, still reachable when
                // --lod-per-cluster is off, still reproducing pre-existing behaviour exactly then.
                if (!clusterDispatched && lodPerClusterEnabled_ && !skinned && haveWorldBox) {
                    if (const auto cit = meshClusterData_.find(mr->mesh); cit != meshClusterData_.end()) {
                        const MeshClusterData& cd = cit->second;
                        trifactor::View view;
                        view.eye = eye_;
                        view.viewProj = viewProj_;
                        view.viewportHeightPx = vpH_;
                        view.verticalFovRadians = radians(60.0f);
                        const f32 worldScale = xformVec(wm, Vec3{1, 0, 0}).size();

                        // THE INSTANCE-LEVEL SHORTCUT -- see aver::trifactor::ClusterAdapt.hpp's own
                        // section for the full derivation and TrifactorTest's own sweep proving it
                        // against the real scan. chooseLevelCached is a handful of float ops (the SAME
                        // call the per-level path below already makes); if provablySingleLevelCut can
                        // PROVE the real O(all-DAG-clusters) scan below would select exactly the whole
                        // of the level it names -- never merely guessed, always proved -- draw that
                        // level's already-resident ladder handle directly and skip the copy, the
                        // per-cluster transform, and the scan entirely. Falls through to the real scan,
                        // unchanged, whenever the proof does not hold (genuinely mixed-level instances,
                        // or a mesh with no matching MeshLodLadder) -- reproducing this path's
                        // pre-existing behaviour exactly for those cases.
                        bool tookShortcut = false;
                        if (const auto lit = meshLods_.find(mr->mesh); lit != meshLods_.end()) {
                            const MeshLodLadder& ladder = lit->second;
                            const Vec3 sphereCenter = (wlo + whi) * 0.5f;
                            const f32 sphereRadius = dist(wlo, whi) * 0.5f;
                            const u32 candidateLevel = trifactor::chooseLevelCached(
                                ladder.errorCm, sphereCenter, sphereRadius, lodErrorThresholdPx_, view);
                            if (candidateLevel < ladder.handles.size() &&
                                trifactor::provablySingleLevelCut(
                                    cd.levelBounds, candidateLevel, sphereCenter, sphereRadius,
                                    cd.maxSphereRadius * worldScale, lodErrorThresholdPx_, view)) {
                                mesh = ladder.handles[candidateLevel];
                                tookShortcut = true;
                                ++lodClusterStats_.instancesShortcut;
                            }
                        }

                        if (!tookShortcut) {
                        // Mesh-local -> world, once per instance per frame, for EVERY cluster of the
                        // mesh's whole DAG at once (all levels) -- the local cut test needs every
                        // cluster's world-space sphere to compare against this frame's camera. Same
                        // uniform-scale approximation the per-level path's informational telemetry
                        // above already uses for radius/axis.
                        std::vector<trifactor::MeshClusterView> worldClusters = cd.clusters;
                        for (trifactor::MeshClusterView& cv : worldClusters) {
                            cv.sphereCenter = xformPoint(wm, cv.sphereCenter);
                            cv.sphereRadius *= worldScale;
                            cv.coneApex = xformPoint(wm, cv.coneApex);
                            cv.coneAxis = xformVec(wm, cv.coneAxis).getSafeNormal();
                        }

                        const trifactor::ClusterCutResult cr = trifactor::selectClusterCut(
                            worldClusters, lodErrorThresholdPx_, view, true /* useFrustum */);

                        ++lodClusterStats_.instancesTested;
                        lodClusterStats_.clustersTested += cr.stats.tested;
                        lodClusterStats_.frustumCulled += cr.stats.frustumCulled;
                        lodClusterStats_.coneCulled += cr.stats.coneCulled;
                        lodClusterStats_.lodRejected += cr.stats.lodRejected;
                        lodClusterStats_.clustersDrawn += cr.stats.drawn;
                        lodClusterStats_.trianglesDrawn += cr.stats.trianglesAfter;
                        if (const auto tIt = meshTris_.find(mr->mesh); tIt != meshTris_.end())
                            lodClusterStats_.trianglesBeforeLod0 += tIt->second;
                        if (cr.stats.distinctLevels > 1) ++lodClusterStats_.instancesMixedLevels;
                        lodClusterStats_.maxDistinctLevelsSeen =
                            std::max(lodClusterStats_.maxDistinctLevelsSeen, cr.stats.distinctLevels);

                        // Sort for a cheap, order-independent "did the cut change since last frame"
                        // comparison -- see ClusterCutCache's own comment. The cut is expected to be
                        // STABLE frame to frame (the camera moves continuously, not by a full LOD
                        // jump every frame), so this is a cache hit most frames once settled.
                        std::vector<u32> selectedIds = cr.drawnIds;
                        std::sort(selectedIds.begin(), selectedIds.end());

                        ClusterCutCache& cache = clusterCutCache_[ent];
                        cache.lastUsedFrame = lodClusterFrame_;
                        if (selectedIds != cache.selectedIds || cache.handle == 0) {
                            // REBUILD: concatenate every selected cluster's precomputed expanded
                            // GLOBAL index list into ONE fresh index buffer, against the SAME shared
                            // vertex array every level of this mesh already uses (cd.verts) -- one
                            // draw call for the whole mixed-LOD cut, never one call per cluster (see
                            // the task report on why that shape avoids needing ExecuteIndirect at
                            // all). Measured, not guessed: real wall time around the real createMesh
                            // call, and the real index/vertex byte counts that upload costs.
                            std::vector<u32> assembled;
                            u64 idxCount = 0;
                            for (u32 cidx : selectedIds)
                                if (cidx < cd.clusterIndices.size()) idxCount += cd.clusterIndices[cidx].size();
                            assembled.reserve(idxCount);
                            for (u32 cidx : selectedIds)
                                if (cidx < cd.clusterIndices.size())
                                    assembled.insert(assembled.end(), cd.clusterIndices[cidx].begin(),
                                                      cd.clusterIndices[cidx].end());

                            const auto t0 = std::chrono::steady_clock::now();
                            const rhi::MeshHandle newHandle = assembled.empty() ? 0 :
                                e.device()->createMesh(cd.verts.data(), (u32)cd.verts.size(),
                                                        assembled.data(), (u32)assembled.size());
                            const auto t1 = std::chrono::steady_clock::now();
                            lodClusterStats_.rebuildMs +=
                                std::chrono::duration<f64, std::milli>(t1 - t0).count();
                            lodClusterStats_.rebuildIndices += assembled.size();
                            ++lodClusterStats_.rebuilds;

                            if (newHandle) {
                                if (cache.handle) e.device()->destroyMesh(cache.handle);
                                cache.handle = newHandle;
                                cache.selectedIds = std::move(selectedIds);
                                ++cache.rebuildCount;
                            }
                            // newHandle == 0 (empty cut this frame, or the device refused): keep
                            // whatever handle the cache already had (fail-safe -- an instance never
                            // draws visibly-wrong geometry because ONE frame's cut came up empty), or
                            // fall through to the LOD-0 whole-mesh handle `mesh` already holds, for
                            // the very first frame where there is no old handle yet either.
                        } else {
                            ++lodClusterStats_.cacheHits;
                        }
                        if (cache.handle) mesh = cache.handle;
                        }   // !tookShortcut
                    }
                } else if (!clusterDispatched && lodSelectEnabled_ && !skinned && haveWorldBox) {
                    if (const auto lit = meshLods_.find(mr->mesh); lit != meshLods_.end()) {
                        const MeshLodLadder& ladder = lit->second;
                        const Vec3 sphereCenter = (wlo + whi) * 0.5f;
                        const f32 sphereRadius = dist(wlo, whi) * 0.5f;   // half the box diagonal:
                                                                          // encloses the box exactly,
                                                                          // same conservative shape
                                                                          // ClusterSelect's own sphere
                                                                          // tests assume.
                        trifactor::View view;
                        view.eye = eye_;
                        view.viewProj = viewProj_;
                        view.viewportHeightPx = vpH_;
                        view.verticalFovRadians = radians(60.0f);   // matches the literal at this
                                                                     // frame's own proj build, above
                        const u32 level = trifactor::chooseLevelCached(
                            ladder.errorCm, sphereCenter, sphereRadius, lodErrorThresholdPx_, view);
                        mesh = ladder.handles[level];

                        ++lodStats_.instancesTested;
                        if (level > 0) ++lodStats_.levelCollapsed;
                        lodStats_.trianglesBeforeLod0 += ladder.triCounts.front();
                        lodStats_.trianglesAfterLevel += ladder.triCounts[level];

                        // Informational cluster-cull telemetry for the CHOSEN level only -- real,
                        // tested selectVisibleClustersWithStats, but its result is not subtracted
                        // from trianglesAfterLevel above: this slice draws the whole chosen level.
                        // See ClusterAdapt.hpp's file header and MeshLodLadder's own comment.
                        //
                        // ladder.clusters[level] holds bounds in MESH-LOCAL space (as authored in the
                        // .ocmesh); view.viewProj/frustum are WORLD space, so a working copy must be
                        // transformed by this instance's world matrix first, or the frustum/cone test
                        // would compare world-space planes against local-space spheres and mean
                        // nothing. Radius/axis use a UNIFORM-scale approximation (length of one
                        // transformed basis vector) -- exact for a uniformly-scaled instance,
                        // conservative-ish otherwise; acceptable for an INFORMATIONAL counter that
                        // never reaches the draw call.
                        if (lodClusterStatsEnabled_ &&
                            level < ladder.clusters.size() && !ladder.clusters[level].empty()) {
                            const f32 worldScale = xformVec(wm, Vec3{1, 0, 0}).size();
                            std::vector<trifactor::ClusterView> worldClusters = ladder.clusters[level];
                            for (trifactor::ClusterView& cv : worldClusters) {
                                cv.sphereCenter = xformPoint(wm, cv.sphereCenter);
                                cv.sphereRadius *= worldScale;
                                cv.coneApex = xformPoint(wm, cv.coneApex);
                                cv.coneAxis = xformVec(wm, cv.coneAxis).getSafeNormal();
                            }
                            const trifactor::SelectionResult sr = trifactor::selectVisibleClustersWithStats(
                                worldClusters, 1e30f /* no LOD collapse: already chosen */, view,
                                true /* useFrustum */);
                            lodStats_.clustersTested += sr.stats.tested;
                            lodStats_.frustumCulled += sr.stats.frustumCulled;
                            lodStats_.coneCulled += sr.stats.coneCulled;
                        }
                    }
                }
#endif
                if (skinnedScene_)
                    if (const rhi::MeshHandle sk = skinnedScene_->drawHandle(ent)) mesh = sk;
#if AVER_MODULE_VOXI
                // Mirrors the depth-prepass walk's own exclusions EXACTLY -- skinned, GPU cluster
                // dispatch, CPU per-cluster -- see that walk's block comment (search "depth prepass
                // phase") for why each is excluded. This must stay in lockstep with the walk above:
                // asking for the LessEqual/no-write pipeline on geometry nothing wrote depth for
                // leaves a hole, and it is exactly this test's job to make sure that never happens.
                {
                    bool prepassEligible = e.device()->depthPrepassEnabled() && !clusterDispatched && !skinned;
#if AVER_MODULE_TRIFACTOR
                    if (prepassEligible && lodMeshShaderEnabled_ && lodMeshPipelineReady_ &&
                        meshClusterGpu_.count(mr->mesh)) prepassEligible = false;
                    if (prepassEligible && lodPerClusterEnabled_ && meshClusterData_.count(mr->mesh))
                        prepassEligible = false;
#endif
                    if (prepassEligible) e.device()->setNextDrawPrepassed(true);
                }
#endif
                // The GPU per-cluster path already dispatched this instance's geometry itself
                // (DispatchMesh, inside dispatchMeshClusters) -- drawing it again here would be a
                // double draw, not a fallback.
                if (!clusterDispatched)
                    e.device()->drawMesh(mesh, &wm.m[0][0], col, metallic, roughness);
                if (sel_ == kSelScene && ent == selEntity_)
                    selectionOutline_ = wm, selectionMesh_ = mesh, hasSelection_ = true;
                ++drawn;
            }
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
            // The pass-2-empty fallback -- see occlusionBuildAndTest's own comment for why this has
            // to run even when there was nothing left to gate this frame: occlusionVisible_ still
            // needs a fresh answer for every entity, or a wall walked INTO front of a previously
            // "visible" entity would never be discovered and that entity would stay in pass 1 (drawn,
            // wastefully but not incorrectly) forever.
            if (occlusionCullEnabled_ && occluder_) occlusionBuildAndTest();
            // Reported on the SAME "power-of-two frame count" cadence D3D12Device's own GPU-timing
            // report uses (RHIResources.hpp's ScopedGpuStat / D3D12Device::collectGpuTiming), so the
            // culled-count line and the "HZB build"/"HZB test" GPU spans it names land at roughly the
            // frame this app was already going to print something at, rather than a second unrelated
            // rhythm the log has to be read against.
            if (occlusionCullEnabled_ && occluder_ && occlusionReportFrames_ &&
                (occlusionReportFrames_ & (occlusionReportFrames_ + 1)) == 0) {
                const f64 pct = occlusionTestedAccum_ ? 100.0 * static_cast<f64>(occlusionCulledAccum_) /
                                                        static_cast<f64>(occlusionTestedAccum_) : 0.0;
                AVER_INFO("[Occlusion] {} of {} tested entities culled ({:.1f}%) over {} frame(s)",
                          occlusionCulledAccum_, occlusionTestedAccum_, pct, occlusionReportFrames_);
            }
#endif
            {
                const f64 walkMs = std::chrono::duration<f64, std::milli>(
                    std::chrono::steady_clock::now() - tWalk0).count();
                if ((sceneWalkReports_ & (sceneWalkReports_ + 1)) == 0)
                    AVER_INFO("[Sandbox] scene walk {:.1f}ms -- {:.1f}ms in cluster dispatch across {} "
                              "drawn ({:.1f}us each), {:.1f}ms in the rest over {} entities",
                              walkMs, dispatchMs, drawn,
                              drawn ? dispatchMs * 1000.0 / static_cast<f64>(drawn) : 0.0,
                              walkMs - dispatchMs, n);
                ++sceneWalkReports_;
            }
            if (drawn != lastSceneDrawn_ || culled != lastSceneCulled_) {
                AVER_INFO("[Sandbox] scene-render: {} spawned CMeshRenderer entit{} drawn, {} frustum-culled",
                          drawn, drawn == 1 ? "y" : "ies", culled);
                lastSceneDrawn_ = drawn;
                lastSceneCulled_ = culled;
            }
#if AVER_MODULE_TRIFACTOR
            // Greppable per the brief's "report the counters" requirement: `[LOD-SELECT]`, only when
            // something in the tuple changed (same log-on-change discipline as scene-render above).
            // trianglesBeforeLod0 vs trianglesAfterLevel is the number that actually predicts frame
            // time -- every triangle in "after" reaches a real drawMesh call this frame. clustersTested/
            // frustumCulled/coneCulled are real telemetry from ClusterSelect's own tested functions,
            // run against the CHOSEN level's meshlets, but are informational only in this slice: see
            // ClusterAdapt.hpp's file header for why they are not (yet) subtracted from trianglesAfterLevel.
            if (lodSelectEnabled_ &&
                (lodStats_.instancesTested != lastLoggedLodStats_.instancesTested ||
                 lodStats_.levelCollapsed != lastLoggedLodStats_.levelCollapsed ||
                 lodStats_.trianglesAfterLevel != lastLoggedLodStats_.trianglesAfterLevel ||
                 lodStats_.frustumCulled != lastLoggedLodStats_.frustumCulled ||
                 lodStats_.coneCulled != lastLoggedLodStats_.coneCulled)) {
                AVER_INFO("[LOD-SELECT] instances={} levelCollapsed={} clustersTested={} "
                          "frustumCulled(info)={} coneCulled(info)={} trisBefore={} trisAfter={}",
                          lodStats_.instancesTested, lodStats_.levelCollapsed, lodStats_.clustersTested,
                          lodStats_.frustumCulled, lodStats_.coneCulled,
                          lodStats_.trianglesBeforeLod0, lodStats_.trianglesAfterLevel);
                lastLoggedLodStats_ = lodStats_;
            }
            // `[LOD-CLUSTER]`, greppable, same log-on-change discipline. `distinctLevelsMax` is the
            // number the task brief calls the proof of the feature: if it never exceeds 1 across a
            // whole run, every instance's cut still collapsed to one level and this is discrete LOD
            // with extra steps, not virtualized geometry, whatever else changed. `rebuilds`/`hits`
            // and `rebuildMs`/`rebuildIdx` are the CPU-assembly cost this design was told to measure,
            // not guess -- real counts and real wall time from the real createMesh calls above.
            // `instancesShortcut` is THE INSTANCE-LEVEL SHORTCUT's own count: instances that never ran
            // the scan at all because provablySingleLevelCut proved it unnecessary -- NOT included in
            // `instancesTested`/`clustersTested` below (those only count instances that actually ran
            // the real scan), so `instancesShortcut + instancesTested` is this frame's true instance
            // total for the per-cluster path.
            if (lodPerClusterEnabled_ &&
                (lodClusterStats_.instancesTested != lastLoggedLodClusterStats_.instancesTested ||
                 lodClusterStats_.clustersDrawn != lastLoggedLodClusterStats_.clustersDrawn ||
                 lodClusterStats_.trianglesDrawn != lastLoggedLodClusterStats_.trianglesDrawn ||
                 lodClusterStats_.instancesMixedLevels != lastLoggedLodClusterStats_.instancesMixedLevels ||
                 lodClusterStats_.instancesShortcut != lastLoggedLodClusterStats_.instancesShortcut ||
                 lodClusterStats_.rebuilds != lastLoggedLodClusterStats_.rebuilds)) {
                AVER_INFO("[LOD-CLUSTER] instances={} instancesShortcut={} clustersTested={} "
                          "frustumCulled={} coneCulled={} lodRejected={} clustersDrawn={} trisBefore={} "
                          "trisDrawn={} instancesMixedLevels={} distinctLevelsMax={} rebuilds={} hits={} "
                          "rebuildIdx={} rebuildMs={:.3f}",
                          lodClusterStats_.instancesTested, lodClusterStats_.instancesShortcut,
                          lodClusterStats_.clustersTested,
                          lodClusterStats_.frustumCulled, lodClusterStats_.coneCulled,
                          lodClusterStats_.lodRejected, lodClusterStats_.clustersDrawn,
                          lodClusterStats_.trianglesBeforeLod0, lodClusterStats_.trianglesDrawn,
                          lodClusterStats_.instancesMixedLevels, lodClusterStats_.maxDistinctLevelsSeen,
                          lodClusterStats_.rebuilds, lodClusterStats_.cacheHits,
                          lodClusterStats_.rebuildIndices, lodClusterStats_.rebuildMs);
                lastLoggedLodClusterStats_ = lodClusterStats_;
            }
            // `[LOD-MESH-SHADER]`, greppable, same log-on-change discipline. `clustersDispatched` is
            // real: the exact clusterCount every dispatchMeshClusters call this frame used, the same
            // number the amplification shader's own gClusterCount reads. `survivors`/`trianglesDrawn`/
            // `distinctLevelsMax` are the CPU-mirrored telemetry LodMeshShaderStats' own comment
            // describes -- sampled every 64 frames, not read back from the GPU this slice.
            if (lodMeshShaderEnabled_ &&
                (lodMeshShaderStats_.instancesTested != lastLoggedLodMeshShaderStats_.instancesTested ||
                 lodMeshShaderStats_.clustersDispatched != lastLoggedLodMeshShaderStats_.clustersDispatched ||
                 lodMeshShaderStats_.survivors != lastLoggedLodMeshShaderStats_.survivors ||
                 lodMeshShaderStats_.instancesMixedLevels != lastLoggedLodMeshShaderStats_.instancesMixedLevels ||
                 lodMeshShaderStats_.instancesShortcut != lastLoggedLodMeshShaderStats_.instancesShortcut)) {
                AVER_INFO("[LOD-MESH-SHADER] pipelineReady={} instances={} clustersDispatched={} "
                          "survivors(sampled)={} trisDrawn(sampled)={} instancesMixedLevels(sampled)={} "
                          "distinctLevelsMax(sampled)={} shortcut(sampled)={}",
                          lodMeshPipelineReady_, lodMeshShaderStats_.instancesTested,
                          lodMeshShaderStats_.clustersDispatched, lodMeshShaderStats_.survivors,
                          lodMeshShaderStats_.trianglesDrawn, lodMeshShaderStats_.instancesMixedLevels,
                          lodMeshShaderStats_.maxDistinctLevelsSeen, lodMeshShaderStats_.instancesShortcut);
                lastLoggedLodMeshShaderStats_ = lodMeshShaderStats_;
            }
#endif
        }
#endif
        // Selection outline: an enlarged wireframe shell over both passes, interactive runs only.
        if (hasSelection_ && maxFrames_ == 0) {
            static constexpr f32 kSelect[4] = {1.0f, 0.62f, 0.12f, 1.0f};   // selection orange
            const Vec3 sp{selectionOutline_.m[3][0], selectionOutline_.m[3][1], selectionOutline_.m[3][2]};
            const f32 camDist = (sp - camPos_).size();
            const f32 grow = 1.0f + std::fmin(0.12f, std::fmax(0.02f, camDist * 0.000009f));
            Mat4 o = selectionOutline_;
            for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) o.m[r][c] *= grow;
            e.device()->setWireframe(true);
            e.device()->drawMesh(selectionMesh_, &o.m[0][0], kSelect, 0.0f, 1.0f);
            e.device()->setWireframe(wireframe_);
        }
        hasSelection_ = false;

        e.device()->setWireframe(false);
        if (showGrid_) {
            const Mat4 g = Mat4::identity();
            e.device()->drawLines(gridMesh_, &g.m[0][0]);
        }
        drawGizmo(e);
#if AVER_MODULE_LANDSCAPE
        drawSculptCursor(e);
#endif
        buildUI(e);
#if AVER_WITH_IMGUI
        uiReg_.endFrame();
#endif
        submitGameUi(e);
        // Before captureCheck, because both use the device's single capture slot and the draw test
        // finishes inside the first dozen frames while the ordinary probe fires near the last.
        skinDrawCheck(e);
        if (refl_ && !refl_->finished()) {
#if AVER_MODULE_VOXI
            // The schedule owns both switches, because the experiment IS the pair of them: the same
            // world change has to be run past the ray tracer and past the cone tracer.
            voxi::Settings vs = voxi::Renderer::get().settings();
            const auto want = refl_->wantRayTracing() ? voxi::Quality::High : voxi::Quality::Off;
            if (vs.rayTracing != want) {
                vs.rayTracing = want;
                voxi::Renderer::get().setSettings(vs);
                voxiRenderer_.setSettings(vs);
            }
#endif
            if (reflBeaconIndex_ >= 0 && reflBeaconIndex_ < (int)objects_.size())
                objects_[reflBeaconIndex_].visible = refl_->beaconVisible();
            refl_->tick(e, vpX_, vpY_, vpW_, vpH_);
        }
        // lastSceneCulled_ is declared only under AVER_MODULE_SCENE (it counts what the scene-render
        // pass above just culled), but skinScene_->tick() is called unguarded -- SkinSceneTest is
        // designed to degrade to a no-op (setup() already returned false with SCENE off), so with no
        // scene there is nothing to have culled and 0 is the value lastSceneCulled_ would hold anyway.
        if (skinScene_ && !skinScene_->finished())
#if AVER_MODULE_SCENE
            skinScene_->tick(e, vpX_, vpY_, vpW_, vpH_, static_cast<u32>(lastSceneCulled_ < 0 ? 0 : lastSceneCulled_));
#else
            skinScene_->tick(e, vpX_, vpY_, vpW_, vpH_, 0u);
#endif
        captureCheck(e);
    }

    // Drives --skin-draw-test, handing it the LIVE viewport rect so its probes can be expressed as
    // fractions of it. Fractions rather than pixels because the rect depends on the DPI and on which
    // panels are open, and a probe that lands on editor chrome reads chrome and reports it as
    // shading -- a trap this repo has already been caught by once.
    void skinDrawCheck(Engine& e) {
        if (!skinDraw_ || skinDraw_->finished()) return;
#if AVER_MODULE_VOXI
        // The test's second experiment holds the pose still and toggles ray tracing instead, because
        // the sun cascade is rasterised from the same posed vertices and so moves with the pose
        // whether or not the acceleration structure was rebuilt -- measured, not assumed.
        const bool want = skinDraw_->wantRayTracing();
        voxi::Settings s = voxi::Renderer::get().settings();
        const auto q = want ? voxi::Quality::High : voxi::Quality::Off;
        if (s.rayTracing != q) {
            s.rayTracing = q;
            voxi::Renderer::get().setSettings(s);
            voxiRenderer_.setSettings(s);
        }
#endif
        skinDraw_->tick(*e.device(), vpX_, vpY_, vpW_, vpH_);
    }

    // Hands the ABI's retained UI draw list to the game UI render feature, once per frame.
    void submitGameUi(Engine& e) {
        (void)e;
        if (!gameUi_) return;

        if (showUiDemo_) drawUiDemo();

        const auto* dl = static_cast<const aver::ui::UiDrawList*>(aver_ui_draw_list());
        if (!dl) return;
        gameUi_->submit(*dl);
    }

    // Draws the placeholder HUD demo through the UI C ABI: bars, crosshair, clipped list, tooltip.
    void drawUiDemo() {
        float vp[4] = {};
        aver_ui_viewport(vp);
        const f32 ox = vp[0], oy = vp[1], sw = vp[2], sh = vp[3];
        if (sw < 80.0f || sh < 60.0f) return;

        aver_ui_push_clip(static_cast<i32>(ox), static_cast<i32>(oy),
                          static_cast<i32>(ox + sw), static_cast<i32>(oy + sh));

        // Colours are 0xAABBGGRR, premultiplied.
        constexpr u32 kPanel   = 0xB0201814;   // near-black, 69% alpha
        constexpr u32 kFrame   = 0xFF3A3226;
        constexpr u32 kHealth  = 0xFF2E4CE8;   // red, in BGR order
        constexpr u32 kStamina = 0xFF3FC8E8;   // amber
        constexpr u32 kInk     = 0xFFE8E4DC;

        aver_ui_set_layer(AVER_UI_LAYER_CONTENT);
        const f32 barW = 260.0f, barH = 14.0f;
        const f32 barX = ox + 32.0f, barY = oy + sh - 240.0f;
        aver_ui_rect(barX - 3, barY - 3, barW + 6, barH * 2 + 12, kPanel);
        aver_ui_rect(barX, barY, barW, barH, kFrame);
        aver_ui_rect(barX + 1, barY + 1, (barW - 2) * uiDemoHealth_, barH - 2, kHealth);
        aver_ui_rect(barX, barY + barH + 6, barW, barH, kFrame);
        aver_ui_rect(barX + 1, barY + barH + 7, (barW - 2) * uiDemoStamina_, barH - 2, kStamina);

        const f32 cx = ox + sw * 0.5f, cy = oy + sh * 0.5f;
        aver_ui_rect(cx - 11, cy - 1, 7, 2, kInk);
        aver_ui_rect(cx + 4,  cy - 1, 7, 2, kInk);
        aver_ui_rect(cx - 1, cy - 11, 2, 7, kInk);
        aver_ui_rect(cx - 1, cy + 4,  2, 7, kInk);

        const f32 pw = 220.0f, ph = 132.0f;
        const f32 px = ox + sw - pw - 32.0f, py = oy + 240.0f;
        aver_ui_set_layer(AVER_UI_LAYER_OVERLAY);
        aver_ui_rect(px, py, pw, ph, kPanel);
        aver_ui_push_clip(static_cast<i32>(px) + 8, static_cast<i32>(py) + 8,
                          static_cast<i32>(px + pw) - 8, static_cast<i32>(py + ph) - 8);
        for (int i = 0; i < 12; ++i) {
            const f32 ry = py + 12.0f + static_cast<f32>(i) * 18.0f - uiDemoScroll_;
            aver_ui_rect(px + 12, ry, pw - 24, 12, (i & 1) ? 0x60E8E4DC : 0x30E8E4DC);
        }
        aver_ui_pop_clip();

        aver_ui_set_layer(AVER_UI_LAYER_TOOLTIP);
        aver_ui_rect(px - 40, py + ph - 24, 96, 20, 0xE0202020);
        aver_ui_rect(px - 38, py + ph - 22, 92, 16, 0xFF6AC46A);

        aver_ui_pop_clip();
    }

    // Tears the editor down: MCP, prefs, physics, UI textures, materials, render features, scripts.
    // The process exit code. Non-zero when a test mode that was ASKED FOR did not pass.
    //
    // Only modes the command line requested are judged: an ordinary editor session must exit 0, and
    // a test that was never started is not a failure. A requested test that never finished IS one --
    // the window being closed early, or a crash-free hang, is exactly the case a script needs to
    // catch, and silence would read as success.
    int exitCode() const override {
        if (skinScene_) {
            if (!skinScene_->finished()) {
                AVER_ERROR("[Skin] --skin-scene-test did not finish; reporting failure rather than "
                           "letting an unfinished run look like a pass");
                return 2;
            }
            if (!skinScene_->passed()) return 1;
        }
        return 0;
    }

    void onShutdown(Engine& e) override {
#if AVER_MODULE_MCP
        mcp_.stop();
#endif
        setLogSink(nullptr, nullptr);
        editor::flushEditorPrefs();
        editor::shutdownActorEditors();
        setMouseCaptured(false);
#if AVER_MODULE_PHYSICS
        aver_phys_shutdown();
#endif
#if AVER_WITH_IMGUI
        if (logoTexture_ || compileIconTexture_ || fileIconsTexture_ || folderIconsTexture_) {
            if (rhi::IResourceFactory* res = e.device()->resources()) {
                res->waitIdle();
                if (logoTexture_) res->destroyTexture(logoTexture_);
                if (compileIconTexture_) res->destroyTexture(compileIconTexture_);
                if (fileIconsTexture_) res->destroyTexture(fileIconsTexture_);
                if (folderIconsTexture_) res->destroyTexture(folderIconsTexture_);
            }
            logoTexture_ = 0; logoUiId_ = 0;
            compileIconTexture_ = 0; compileIconUiId_ = 0;
            fileIconsTexture_ = 0; fileIconsUiId_ = 0;
            folderIconsTexture_ = 0; folderIconsUiId_ = 0;
        }
#endif
#if AVER_MODULE_PBR
        releaseProjectMaterials();
        textureFactory_ = nullptr;
#endif
#if AVER_MODULE_SR
        // DETACH FROM THE DEVICE FIRST, THEN DESTROY -- in that order, or not at all. The comment
        // this replaced said resetting the unique_ptrs here (rather than leaving it to their own
        // destructors after onShutdown returns) was safe because "e.device() is still known good",
        // which is true but answers the wrong question: it is `dev->upscaler()` -- a RAW pointer
        // D3D12Device keeps, set by ensureEdgeAaUpscaler/ensureAverSrUpscaler's own
        // applyUpscalerSlot() -- that has to stop pointing at this object before the object goes
        // away, not the device's own liveness. --edge-aa's first --frames run crashed (SIGSEGV) AT
        // PROCESS EXIT with the tree already fully logged and the screenshot already on disk: the
        // measurement was never in question, only teardown order was wrong. Whichever of the two
        // upscalers applyUpscalerSlot last handed to the device is the dangling one; clearing the
        // slot unconditionally, for BOTH, before either reset() runs, is what closes it for both --
        // this is the exact bug clearAverSrUpscaler(dev) exists to prevent and was never called
        // anywhere in this file.
        e.device()->setUpscaler(nullptr);
        averSrUpscaler_.reset();
        edgeAaUpscaler_.reset();
#endif
        if (gameUi_) {
            e.device()->removeRenderFeature(gameUi_);
            delete gameUi_;
            gameUi_ = nullptr;
        }
        if (skinSelfTest_) {
            e.device()->removeRenderFeature(skinSelfTest_.get());
            skinSelfTest_.reset();
        }
        if (ptFurnace_) {
            e.device()->removeRenderFeature(ptFurnace_.get());
            ptFurnace_.reset();
        }
        // Routed through the same reconciler the editor's live toggle uses (rather than a hand-
        // written removeRenderFeature()+reset() here) so process-exit teardown and a user-driven
        // "turn it off" are provably the same code path, not two that have to be kept in sync by
        // hand. onShutdown() runs well outside any frame, so this is exactly as safe as its usual
        // onUpdate() call site -- see syncPtSceneView()'s own comment for why that matters at all.
        ptSceneViewWantEnabled_ = false;
        syncPtSceneView(e.device());
        if (skinDraw_) {
            e.device()->removeRenderFeature(skinDraw_.get());
            skinDraw_.reset();
        }
        // skinnedScene_ is declared only under AVER_MODULE_SCENE (it is the scene join, meaningless
        // without a world to join to), but this teardown block was unguarded.
#if AVER_MODULE_SCENE
        if (skinnedScene_) {
            e.device()->removeRenderFeature(skinnedScene_.get());
            skinnedScene_.reset();
        }
#endif
#if AVER_MODULE_VOXI
        if (voxiAttached_) { e.device()->removeRenderFeature(&voxiRenderer_); voxiAttached_ = false; }
        voxiRenderer_.shutdown();
#else
        (void)e;
#endif
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
        // VERIFICATION-ONLY: prints the CPU simulation cost this run measured, once, before teardown
        // -- see particleTickAccumSec_'s own comment.
        if (particleTickFrames_ > 0) {
            u32 totalLive = 0, liveEmitters = particles::particleSystem().liveEmitters();
            particles::particleSystem().forEachEmitter([&](const particles::EmitterView& ev) {
                totalLive += ev.particles ? static_cast<u32>(ev.particles->size()) : 0;
            });
            AVER_INFO("[Particles] CPU tick: {:.2f}us/frame avg over {} frame(s) ({:.3f}ms total), "
                      "{} emitter(s) / {} live particle(s) at shutdown",
                      (particleTickAccumSec_ / static_cast<f64>(particleTickFrames_)) * 1e6,
                      particleTickFrames_, particleTickAccumSec_ * 1e3, liveEmitters, totalLive);
        }
        if (particlesAttached_) { e.device()->removeRenderFeature(&particleRenderer_); particlesAttached_ = false; }
        particleRenderer_.shutdown();
#endif
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
        if (occluder_) {
            if (rhi::IResourceFactory* occRes = e.device()->resources())
                aver::occlusion::destroyOcclusionCuller(*occRes, occluder_);
            occluder_ = nullptr;
        }
#endif
#if AVER_MODULE_SCRIPTING
        scripts_.shutdown();
#endif
        AVER_INFO("[Sandbox] shutdown");
    }
    void setVSyncOff(bool off) { vsyncOffRequested_ = off; }               // --no-vsync
    // --lod-select [px] / --no-lod-select: virtualized-geometry LOD selection
    // (aver::trifactor::ClusterAdapt) for meshes the Cook wrote coarser LOD levels for. `thresholdPx`
    // is the pixel budget passed straight through to chooseLevelCached/screenSpaceErrorPx.
    //
    // NOW ON BY DEFAULT. It was off, on the reasoning that off "reproduces pre-existing behaviour
    // EXACTLY" -- true, and the pre-existing behaviour was drawing every instance at LOD 0 at every
    // distance. Measured on the Electric Dreams camera at --no-vsync: 102.7ms median / 153.0ms p90
    // with selection off, 76.7ms / 127.3ms with it on. A 1px error budget is not a quality decision
    // anyone would make deliberately in the other direction.
    void setLodSelect(bool on, f32 thresholdPx) {
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
        lodSelectEnabled_ = on;
        lodErrorThresholdPx_ = thresholdPx;
#else
        (void)on; (void)thresholdPx;
#endif
    }
    // --lod-cluster-stats: see lodClusterStatsEnabled_'s own comment for why this is a second,
    // separately-gated flag rather than folded into setLodSelect.
    void setLodClusterStats(bool on) {
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
        lodClusterStatsEnabled_ = on;
#else
        (void)on;
#endif
    }
    // --lod-per-cluster [px]: turns on PER-CLUSTER virtualized-geometry selection
    // (aver::trifactor::ClusterAdapt's selectClusterCut), replacing --lod-select's per-LEVEL choice
    // for an instance entirely when both are given (this one wins -- see the draw loop's branch).
    // OFF (the default) leaves --lod-select's per-level path, or no LOD selection at all, completely
    // unaffected -- same "before/after is one flag on the same build" contract as --lod-select.
    void setLodPerCluster(bool on, f32 thresholdPx) {
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
        lodPerClusterEnabled_ = on;
        if (on) lodErrorThresholdPx_ = thresholdPx;   // shares the one pixel-budget knob with --lod-select
#else
        (void)on; (void)thresholdPx;
#endif
    }
    // --lod-mesh-shader [px]: the GPU per-cluster path -- an amplification shader runs the SAME local
    // cut test as --lod-per-cluster's CPU reference, one thread per cluster, and DispatchMesh's the
    // survivors instead of the CPU assembling one index buffer per cut change. Wins over
    // --lod-per-cluster and --lod-select for an instance whose mesh has GPU cluster data uploaded AND
    // this run's device actually built the pipeline (mesh-shader tier > 0) -- see the draw loop's
    // branch order. Falls back to whichever of the other two flags is also set, per-instance, if
    // either the mesh has no GPU cluster buffers yet or the pipeline never came up: house rule 6's
    // "degrade, not crash" at the feature level, not just the shader-compile level.
    // --lod-mesh-shader / --no-lod-mesh-shader. Records an EXPLICIT choice, which suppresses the
    // caps-driven default in onInit -- see the comment there for why the default is on.
    void setLodMeshShader(bool on, f32 thresholdPx) {
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
        lodMeshShaderRequest_ = on ? 1 : 0;
        lodMeshShaderEnabled_ = on;
        if (on) lodErrorThresholdPx_ = thresholdPx;   // shares the one pixel-budget knob with the others
#else
        (void)on; (void)thresholdPx;
#endif
    }
    void setUiDemo(bool on) { showUiDemo_ = on; }                          // --ui-demo
    void setOpenAsset(std::string p) { openAsset_ = std::move(p); }        // --open-asset
    // --graph-select <nodeId>: fired one frame after --open-asset opens (frameNo_ > 6, one past the
    // > 5 gate that block itself uses), so a --frames/--screenshot capture can prove a details/
    // inspector panel renders a real, populated node -- no human clicking the canvas. Same shape as
    // every other CLI test-proof flag in this file (--undo-test, --keybind-test, --hud-preview): a
    // plain setter here, the actual work happens once in onGui() below.
    void setGraphSelectNode(std::string id) { graphSelectNode_ = std::move(id); }
    // --graph-tab viewport: brings the graph tab's inner Viewport tab to the front, on the same
    // frame budget --graph-select uses. Only "viewport" does anything; Event Graph is already the
    // one in front, so there is nothing for the other value to do.
    void setGraphTab(std::string tab) { graphTab_ = std::move(tab); }
    void setInputProbe(bool on) { inputProbe_ = on; }
    void setAutoCompile(bool on) { autoCompile_ = on; }   // --auto-compile, and the Tools menu
    void setFocusLevelAt(int frame) { focusLevelAt_ = frame; }   // --focus-level-at <N>
    // --chunk-stream [N]: frames left before setChunkStreamingEnabled(true) fires on its own. Member
    // and setter are unguarded (like focusLevelAt_) even though the effect is AVER_MODULE_SCENE-only;
    // see the countdown in onUpdate.
    void setChunkStreamAuto(int framesIn) { chunkStreamAutoFrames_ = framesIn; }
    // --drone-graph <relPath>. Unlike setChunkStreamAuto/setDroneAuto above, `droneGraphRel_` itself
    // is guarded `#if AVER_MODULE_SCENE` at its declaration (it names a .ocgraph the SCENE module
    // spawns), so this setter must be guarded too -- SAME PRE-EXISTING SCOPING BUG as
    // setFogMatchToStreamRadius below, fixed the same way while verifying this task's own
    // -DAVER_MODULE_SCENE=OFF build (house rule 6). Not part of the LOD-select/mesh-cluster work;
    // flagged separately in this task's report.
#if AVER_MODULE_SCENE
    void setDroneGraph(std::string relPath) { droneGraphRel_ = std::move(relPath); }
#else
    void setDroneGraph(std::string) {}
#endif

    // --landscape <path>: an explicit .ocland override. Wins over the convention loadLandscapeForLevel
    // uses (the loaded level's own path with its extension swapped to .ocland) the next time a level
    // loads. Exists so a --frames capture run can prove the render and per-frame LOD-selection path
    // draws real terrain without a level file that names one and without a human clicking anything.
    // Unguarded like setChunkStreamAuto/setDroneAuto above: with the module off, loadLandscapeForLevel
    // itself does not exist, and nothing else reads this field.
    void setLandscapePath(std::string path) { landscapeCliOverride_ = std::move(path); }

    // --fog-match. A negative opacity means "leave the target where it is" and just switch matching
    // on, so `--fog-match` alone uses the panel's own default rather than silently redefining it.
    // PRE-EXISTING SCOPING BUG, fixed in passing while verifying THIS task's own -DAVER_MODULE_SCENE=OFF
    // build (house rule 6): unlike setChunkStreamAuto/setDroneAuto right above (deliberately unguarded,
    // per their own comments, because the FIELDS they touch are unguarded too), this setter referenced
    // matchFogToStreamRadius_/fogMatchTargetOpacity_, which ARE guarded `#if AVER_MODULE_SCENE` at
    // their declaration -- so the unguarded setter simply failed to compile with the module off. Not
    // part of the LOD-select feature; flagged separately in this task's report.
#if AVER_MODULE_SCENE
    void setFogMatchToStreamRadius(bool on, f32 targetOpacity) {
        matchFogToStreamRadius_ = on;
        if (targetOpacity > 0.0f) fogMatchTargetOpacity_ = std::clamp(targetOpacity, 0.05f, 0.99f);
    }
#else
    void setFogMatchToStreamRadius(bool, f32) {}
#endif
    // --drone [N]: frames left before setDroneEnabled(true) fires on its own, same shape as
    // --chunk-stream. Member and setter are unguarded for the same reason chunkStreamAutoFrames_ is
    // (the countdown in onUpdate is what's actually AVER_MODULE_SCENE-gated).
    void setDroneAuto(int framesIn) { droneAutoFrames_ = framesIn; }
    // --undo-test [N]: frames left before runUndoTest() fires and the process exits, same shape as
    // --drone/--chunk-stream. See runUndoTest()'s own comment for what it actually proves.
    void setUndoTestAuto(int framesIn) { undoTestAutoFrames_ = framesIn; }
    // --keybind-test write|read [N]: frames left before runKeybindPersistTest(mode) fires and the
    // process exits. See that function's own comment for what the two modes prove between them.
    void setKeybindTestAuto(std::string mode, int framesIn) {
        keybindTestMode_ = std::move(mode); keybindTestAutoFrames_ = framesIn;
    }
    void setShowEditorPrefs(bool on) { if (on) showEditorPrefs_ = true; }   // --editor-prefs
    // --scroll-prefs-to-keybinds: see the one-shot flag's own comment in buildEditorPrefs().
    void setScrollPrefsToKeybinds(bool on) { scrollPrefsToKeybinds_ = on; }
    void setHudTest(int idx) { hudTest_ = idx; }   // --hud-preview <index>
    void setSaveProject(bool on) { saveProject_ = on; }   // --save-project
    // Queues one Content Browser import to run on startup. --import <src> <destDir>.
    void setImportOnce(std::string src, std::string dst) { importSrc_ = std::move(src); importDst_ = std::move(dst); }
    bool* autoCompileFlag() { return &autoCompile_; }

    // Turns clouds on, optionally at the given coverage. --clouds [coverage].
    void setClouds(f32 coverage) {
        sky_.cloudsEnabled = true;
        if (coverage >= 0.0f) sky_.cloudCoverage = coverage;
    }
    // Selects the physical sky and optionally moves the sun's elevation. --sky-physical [elevation].
    void setSkyPhysical(f32 elevationDeg) {
        sky_.model = rhi::SkyModel::Physical;
        if (elevationDeg > -90.0f) {
            f32 elev = 0.0f, azim = 0.0f;
            sky_.sunAngles(elev, azim);
            sky_.setSunAngles(elevationDeg, azim);
        }
    }
    void setSkyAuthored() { sky_.model = rhi::SkyModel::Authored; }   // --sky-authored
    // Sets exposure, bloom intensity and auto-exposure. --exposure / --bloom / --auto-exposure.
    void setPost(f32 exposure, f32 bloomIntensity, bool autoExposure) {
        post_.exposure = exposure;
        post_.bloomIntensity = bloomIntensity;
        if (autoExposure) post_.autoExposure = true;
    }

    // Disables auto-exposure for a capture run unless the run asked for it.
    void applyCaptureExposureRule(bool explicitlyRequested) {
        if (maxFrames_ != 0 && !explicitlyRequested) post_.autoExposure = false;
    }
    void setFocusVoxi(bool b) { focusVoxi_ = b ? 4 : 0; } // --project-settings
    // --project-settings-page N: verification-only, jumps straight to sub-page N (see
    // settingsPage_'s own comment for the index) instead of leaving a screenshot script to navigate
    // a docked window it cannot click. Implies --project-settings.
    void setProjectSettingsPage(int page) { focusVoxi_ = 4; settingsPage_ = page; }
    // Opens a drawer fully open on startup, optionally in a Content subfolder. --drawer.
    void setDrawerOpen(int which, std::string sub) {
        if (!which) return;
        drawer_ = drawerShown_ = which == 2 ? Drawer::Log : Drawer::Content;
        drawerAnim_ = 1.0f;
        drawerStartSub_ = std::move(sub);
    }
    void setFocusScript(bool b) { tools_.armNewScript(b); }  // --new-script
    void setFocusTools(bool b) { tools_.armToolsMenu(b); }   // --tools-menu
    void setFocusCompileMenu(bool b) { tools_.armCompileMenu(b); }   // --compile-menu
#if AVER_MODULE_MCP
    void setMcpPort(u16 p) { mcpPort_ = p; }
#endif
    void setSkinTest() { skinTest_ = true; }                                       // --skin-test
    void setSkinDrawTest() { skinDrawTest_ = true; }                               // --skin-draw-test
    void setParticleTest() { particleTest_ = true; }                               // --particle-test
    // --no-particle-gi: the A/B toggle particles DECIDED 4's own proof needs -- same scene, same Voxi
    // volume, only whether particleRenderer_.setGiSeam is ever called differs. Distinct from --no-gi
    // (which stops the volume from being BUILT at all) and --no-gi-cone (which stops the OPAQUE
    // scene's own cone-trace read) -- this one leaves both of those alone and only ever touches
    // whether PARTICLES sample the (otherwise unaffected) result.
    void setNoParticleGi() { noParticleGi_ = true; }                               // --no-particle-gi
    // --particle-stress <N> <M>: verification-only, see particleStressEmitters_'s own comment.
    void setParticleStress(int emitters, int maxParticles) {
        particleStressEmitters_ = emitters;
        particleStressMaxParticles_ = maxParticles;
    }
    void setParticleStressSecondEmitter() { particleStressSecondEmitter_ = true; }   // --particle-stress2
    void setReflTest() { reflTest_ = true; }                                       // --refl-test
    void setFurnaceTest() { furnaceTest_ = true; }                                 // --furnace-test
    void setFurnaceSun() { furnaceTest_ = true; furnaceSun_ = true; }              // --furnace-sun
    // --sun-angle DEG: the sun's ANGULAR DIAMETER. Not a look control -- it is what sets how wide
    // a ray-traced penumbra is, and at the real 0.545 degrees that penumbra is narrower than a
    // pixel at contact distances. A gate that wants to sample a partially-occluded ray-traced
    // pixel has to widen the source until the transition is several pixels across.
    void setSunAngle(f32 deg) { sunAngle_ = deg; }
    // --pt-furnace: the furnace measured through the PATH TRACER rather than through the raster
    // shading model. It implies --furnace-test because the furnace is a property of the SKY, and
    // that is where the flag puts it -- the tracer reads the same averFurnaceL() every other
    // shading path does.
    void setPtFurnaceTest() { furnaceTest_ = true; ptFurnaceTest_ = true; }        // --pt-furnace
    // --pt-scene seeds the WANT flag syncPtSceneView() reconciles every frame; see that function's
    // own comment for why the actual registration happens there and not here.
    void setPtSceneView() { ptSceneViewWantEnabled_ = true; }   // --pt-scene
    // --pt-scene-toggle-on/--pt-scene-toggle-off [N]: see ptSceneToggleOnAutoFrames_'s own comment.
    void setPtSceneToggleOnAuto(int framesIn)  { ptSceneToggleOnAutoFrames_  = framesIn; }
    void setPtSceneToggleOffAuto(int framesIn) { ptSceneToggleOffAutoFrames_ = framesIn; }
    void setSkinSceneDir(std::string d) { skinSceneDir_ = std::move(d); }          // --skin-scene-test <dir>
    void setFocusCompile(bool b) { tools_.armCompile(b); }   // --compile-scripts
    void setFocusReload(int frames) { if (frames > 0) tools_.armReload(frames); } // --reload-scripts [N]
    void setMsaaOverride(int n) { msaaOverride_ = n; }   // --msaa N
    void setDepthPrepassOverride(bool on) { depthPrepassOverride_ = on; }   // --depth-prepass
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
    // --occlusion-cull: see occlusionCullEnabled_'s own member comment. Unset (the default) never
    // reorders the entity walk, never calls occluder_ at all, and reproduces today's frame exactly --
    // the same "off is a no-op, byte for byte" contract depthPrepassOverride_ carries just above.
    // Guarded the same as the member it assigns (occlusionCullEnabled_, below): with the module OFF,
    // that member does not exist, so this setter cannot either -- see the CLI parse site's own
    // #if AVER_MODULE_OCCLUSION for the only caller, which is guarded identically.
    void setOcclusionCullOverride(bool on) { occlusionCullEnabled_ = on; }
#endif
    void setGiOverride(int q, bool dbg) { giOverride_ = q; giDebugView_ = dbg; } // --gi / --gi-debug
    void setGiForceOff(bool off) { giForceOff_ = off; }                        // --no-gi
    // --no-gi-cone: the A/B measurement toggle from VoxiRenderer::setConeTraceEnabled's own comment.
    // DELIBERATELY NOT --no-gi (which also stops the volume from being built -- see that setter's
    // comment for why the two must stay separate): this flag alone turns off just the per-pixel
    // cone-trace READ so a `--frames N` run with it and one without it, same camera, differ in the
    // "scene draw" GPU span by exactly the trace's own cost and nothing upstream of it.
    void setGiConeTraceOff(bool off) { giConeTraceOff_ = off; }                // --no-gi-cone
    void setRtOverride(int q) { rtOverride_ = q; }                              // --rt
    void setRtForceOff(bool off) { rtForceOff_ = off; }                         // --no-rt
    void setRtRays(int n) { rtRaysOverride_ = n; }                              // --rt-rays N
    void setRtPixelsPerRay(int n) { rtPixelsPerRayOverride_ = n; }              // --rt-pixels-per-ray N
    void setRtShadowDenoise(int n) { rtShadowDenoiseOverride_ = n; }            // --rt-shadow-denoise N
    void setGiUpdateInterval(int n) { giUpdateIntervalOverride_ = n; }          // --gi-update-interval N
    void setRenderScale(f32 s) { renderScaleOverride_ = s; }                    // --render-scale F
#if AVER_MODULE_SR
    void setAverSrQuality(aver::sr::Quality q) { averSrQuality_ = q; }          // --aversr LEVEL

    // Constructs SpatialUpscaler against `dev`'s resource factory if it is not already built.
    // Idempotent -- cheap to call every time the quality combo changes, not just once. See
    // logAverSrActive()'s comment for what constructing it does and does not buy today.
    void ensureAverSrUpscaler(rhi::IDevice* dev) {
        if (!dev) return;
        if (!averSrUpscaler_) {
            if (rhi::IResourceFactory* res = dev->resources())
                averSrUpscaler_ = std::make_unique<aver::sr::SpatialUpscaler>(*res);
        }
        // HANDED TO THE DEVICE, which is the step that was missing: it was constructed, correct
        // against the seam, and reachable from the editor -- and nothing ever called execute(),
        // because IDevice had no slot to put it in. It does now.
        //
        // NULL WHEN Off, and that is the whole of how the bit-identical invariant is kept: the
        // backend branches on upscaler() != nullptr, so Off takes the untouched single-pass
        // composite path a build without this module would take.
        //
        // Non-owning on the device's side -- averSrUpscaler_ outlives it here, and the device is
        // told nullptr before this object goes away (see clearAverSrUpscaler).
        applyUpscalerSlot(dev);
    }

    // Detaches before destruction, so the device can never hold a dangling upscaler.
    void clearAverSrUpscaler(rhi::IDevice* dev) {
        if (dev) dev->setUpscaler(nullptr);
    }

    void setEdgeAaOverride(bool on) { edgeAaEnabled_ = on; }   // --edge-aa

    // Picks whichever of --edge-aa / --aversr should actually be bound to the device's ONE upscaler
    // slot -- see edgeAaEnabled_'s own comment on why only one can run at a time. Every call site
    // that used to hand the device an upscaler directly now goes through this instead, so the two
    // features can never race to silently overwrite each other's choice.
    void applyUpscalerSlot(rhi::IDevice* dev) {
        if (!dev) return;
        if (edgeAaEnabled_ && edgeAaUpscaler_) { dev->setUpscaler(edgeAaUpscaler_.get()); return; }
        dev->setUpscaler(averSrQuality_ == aver::sr::Quality::Off ? nullptr : averSrUpscaler_.get());
    }

    // Constructs FxaaResolve against `dev`'s resource factory if it is not already built, then hands
    // it to the device through applyUpscalerSlot() -- the same idempotent shape as
    // ensureAverSrUpscaler just above, for the same rhi::IUpscaler seam, with a different algorithm
    // behind it. Off (edgeAaEnabled_ never set) never calls this at all.
    void ensureEdgeAaUpscaler(rhi::IDevice* dev) {
        if (!dev) return;
        if (!edgeAaUpscaler_) {
            if (rhi::IResourceFactory* res = dev->resources())
                edgeAaUpscaler_ = std::make_unique<aver::sr::FxaaResolve>(*res);
        }
        applyUpscalerSlot(dev);
        if (edgeAaUpscaler_)
            AVER_INFO("[AverSR] '{}' handed to the device (--edge-aa)", edgeAaUpscaler_->name());
        else
            AVER_WARN("[AverSR] --edge-aa requested but FxaaResolve could not be constructed "
                      "(no resource factory)");
    }

    // Logs the [AverSR] brand-tag line docs/AVERSR.md's naming table specifies, plus the one honest
    // caveat this stage leaves open: rhi::IDevice has no setUpscaler()/upscaler() hook yet (see
    // modules/render.sr/README.md "Backend wiring"), so nothing on the present path ever calls
    // SpatialUpscaler::execute(). The render-scale change is real and already measurable through
    // --render-scale; SpatialUpscaler is constructed, correct against the rhi::IUpscaler seam, and
    // reachable from the editor for the first time, but its resample pass is not yet what produces
    // the pixels on screen -- what is on screen is still the backend's own bilinear render-scale
    // resize. Closing that gap needs a backend to read device->upscaler() from its own
    // composite/present step instead of that resize; modules/rhi.d3d12/src/D3D12Device.cpp is where,
    // and it is out of this change's file ownership.
    void logAverSrActive(rhi::IDevice* dev) {
        if (!dev) return;
        AVER_INFO("[AverSR] {}: render scale {:.2f}{}", aver::sr::qualityName(averSrQuality_),
                  dev->renderScale(),
                  averSrUpscaler_ ? "" : " (SpatialUpscaler not constructed -- no resource factory)");
        // The old warning here said SpatialUpscaler was "constructed but not yet reachable from
        // the present path -- rhi::IDevice has no upscaler hook". That hook exists now and the
        // backend logs when it actually runs, so this would have been a lie the moment it fired.
        if (averSrUpscaler_ && averSrQuality_ != aver::sr::Quality::Off)
            AVER_INFO("[AverSR] {} handed to the device; the backend reports when it upscales",
                      averSrUpscaler_->name());
    }

    // Applies one AverSR quality level from the render-settings combo: the docs/AVERSR.md
    // render-scale table through the SAME rhi::IDevice::setRenderScale the slider next to the combo
    // already edits. Off resets the scale to native and drops any constructed upscaler, so switching
    // back to Off is bit-identical to never having touched the combo at all.
    void applyAverSrQuality(rhi::IDevice* dev, aver::sr::Quality q) {
        averSrQuality_ = q;
        if (!dev) return;
        if (q == aver::sr::Quality::Off) {
            averSrUpscaler_.reset();
            dev->setRenderScale(1.0f);
            return;
        }
        dev->setRenderScale(aver::sr::renderScaleFor(q));
        ensureAverSrUpscaler(dev);
        logAverSrActive(dev);
    }
#endif

    // Reconciles ptSceneView_ (the ACTUAL registration) with ptSceneViewWantEnabled_ (what --pt-scene
    // at startup, or the editor's own Path Tracing settings-page Quality combo -- voxi::Settings::
    // pathTracing != Off -- at any later frame, most recently asked for). Idempotent: a call that
    // finds ptSceneView_ already in the wanted state does nothing, which is what makes it free to
    // call every single frame rather than only on a change.
    //
    // CALLED FROM ONUPDATE() ONLY, and never from inside buildUI()/onRender() where the settings
    // combo itself lives -- see Engine::frameStep(): onUpdate() runs BEFORE device_->beginFrame(), so
    // this is the one point in the loop where nothing is mid-recording. That matters because
    // suppressesScene() is read LIVE, once per drawMesh() call, all through onRender()
    // (D3D12Device::drawMesh) -- mutating features_ while that loop is still running (which is
    // exactly what changing the combo mid-onRender would do, since buildUI() runs after this frame's
    // own scene draws but still inside the same open command list) would let one frame's draws
    // disagree with each other about whether the scene is suppressed. Deferring the actual mutation
    // to the NEXT onUpdate() avoids the question rather than reasoning through it.
    //
    // addRenderFeature()/removeRenderFeature() are themselves the two RHI calls this whole design
    // rests on: addRenderFeature() calls onRenderTargetsChanged() immediately, against the device's
    // CURRENT scene targets (D3D12Device::addRenderFeature), so a feature turned on mid-session
    // builds its present pipeline against THIS session's swapchain, never a stale one from process
    // start -- "build resources against the current render targets" is free, not something this
    // function has to arrange. removeRenderFeature() is a plain vector erase (D3D12Device::
    // removeRenderFeature) with no waitIdle and no special-cased teardown; the object's own
    // destroyBuffer/destroyPipeline/destroyBindingSet calls (run via ~PtSceneView, through reset()
    // below) each retire their resource behind the graphics queue's fence rather than freeing it
    // immediately (D3D12ResourceFactory::retire), so releasing GPU objects a frame or two still in
    // flight might be reading is safe. The one gap that is NOT closed here: PathTracer leaks every
    // TLAS it built (the RHI has no destroyTlas at all -- see PathTracer::shutdown()'s own comment),
    // so toggling this on and off repeatedly in one long editor session leaks one TLAS per re-arm.
    // Small, bounded by how often the STATIC scene actually changes while this is on, and a
    // pre-existing RHI gap rather than something this change introduces -- but real, and worth fixing
    // in the RHI before this feature sees heavy toggling in practice.
    void syncPtSceneView(rhi::IDevice* dev) {
        if (!dev) return;
        if (ptSceneViewWantEnabled_ == (ptSceneView_ != nullptr)) return;

        if (ptSceneViewWantEnabled_) {
            ptSceneView_ = std::make_unique<aver::pt::PtSceneView>();
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
            // THE HOST RESOLVES THE MATERIAL, because the host is the only thing that knows it bound
            // one -- the same division applyLandscapeSurface() uses, and the reason Aver.Render.
            // PathTracer can link Aver.RHI and Aver.Core alone. The tracer forwards the opaque
            // binding it was handed; this decides what it is.
            //
            // ownsBindingSet() is the IDENTITY test, not a shape test: a block that merely happens to
            // be sizeof(MaterialConstants) is not a material, and nothing in the RHI tags a binding
            // with its type. The size is still checked, but as a corroborating assertion after
            // identity has already been established, never as the test itself.
            //
            // AND THE FALLBACK SET MEANS "NOT AUTHORED". The scene loop calls setDrawBinding()
            // unconditionally once the material system is ready, passing ms.bindingSet(authored) --
            // with `authored` zero for any surface painted only through the legacy surfaceLooks_
            // palette, which resolves to fallbackSet_ and a fallbackConstants_ whose baseColorFactor
            // is {1,1,1,1}. Reading that would render every non-authored surface white and throw away
            // the look's real colour, so those draws must fall through to the per-draw baseColor.
            ptSceneView_->setAlbedoResolver(
                [this](aver::rhi::BindingSetHandle set, const void* constants, aver::u32 bytes,
                       aver::f32 outAlbedo[3]) -> bool {
                    if (!set || !constants || bytes != sizeof(pbr::MaterialConstants)) return false;
                    pbr::MaterialSystem& ms = voxiRenderer_.materials();
                    if (!ms.ready()) return false;
                    if (set == ms.fallbackBindingSet()) return false;   // un-authored: keep the look's colour
                    if (!ms.ownsBindingSet(set)) return false;          // not one of ours at all
                    const auto* mc = static_cast<const pbr::MaterialConstants*>(constants);
                    outAlbedo[0] = mc->baseColorFactor[0];
                    outAlbedo[1] = mc->baseColorFactor[1];
                    outAlbedo[2] = mc->baseColorFactor[2];
                    return true;
                });
#endif
            if (ptSceneView_->init(*dev)) {
                dev->addRenderFeature(ptSceneView_.get());
                ptSceneViewUnavailable_ = false;
                AVER_INFO("[PT] scene view enabled");
            } else {
                AVER_ERROR("[PT] scene view unavailable on this device");
                ptSceneView_.reset();
                ptSceneViewUnavailable_ = true;
                // Don't retry every frame. NOTE this does NOT reach back into voxi::Settings::
                // pathTracing -- syncPtSceneView() has no Voxi dependency at all (it must keep
                // working when AVER_MODULE_VOXI is off, since --pt-scene does not need Voxi), so the
                // settings-page combo can be left showing a stale non-Off value after this; see its
                // own BeginDisabled(...||ptSceneViewUnavailable_) for how the UI stays honest anyway.
            }
        } else {
            dev->removeRenderFeature(ptSceneView_.get());
            ptSceneView_.reset();
            AVER_INFO("[PT] scene view disabled; raster scene restored");
        }
    }

    void setFrameTimeReport(bool on) { frameTimeReport_ = on; }                 // --frame-time
    void setMsOverride(bool on) { msOverride_ = on; }                           // --ms
    void setProbe(u32 x, u32 y) { probeX_ = x; probeY_ = y; }                    // --probe X Y
    void setProbeRel(f32 u, f32 v) { probeU_ = u; probeV_ = v; }                 // --probe-rel U V
    // --cam X Y Z PITCH YAW: places the viewport camera outright, cm and degrees, pitch 0 level.
    // A capture tool, not a feature: every other way into this camera either frames the level (always
    // pitch -31 deg, horizon just off the top edge) or needs a real mouse, so nothing headless could
    // aim at the sky, and the sky is the one thing no gate covers. Applied last, after level framing.
    // --cam-wobble DEG PERIOD: swing the yaw sinusoidally about wherever the camera is aimed.
    //
    // A SINE THAT RETURNS TO ZERO, not a one-way pan, and that is the whole point. Measuring what
    // camera motion does to a temporally amortised effect means comparing a moving run against a
    // still one AT THE SAME PIXEL -- and a one-way pan ends somewhere else, so the probe lands on
    // different geometry and the two numbers are not comparable. sin() is zero at every whole
    // multiple of the period, so a run whose frame count lands on one ends aimed exactly where it
    // started: same surface under the probe, same lighting, and the only thing left different is
    // the history the motion built up.
    //
    // Driven off the engine's frame COUNTER, never the clock, so the path is identical on every
    // machine and every run -- the gate oracle's whole story rests on captures being reproducible.
    void setCamWobble(f32 degrees, i32 periodFrames) {
        camWobbleDeg_ = degrees;
        camWobblePeriod_ = periodFrames > 0 ? periodFrames : 0;
    }
    void setCamera(Vec3 pos, f32 pitchDeg, f32 yawDeg) {
        camOverride_ = true;
        camPosOverride_ = pos;
        pitchOverride_ = pitchDeg * 0.01745329252f;
        yawOverride_   = yawDeg   * 0.01745329252f;
    }
    void setScriptsDir(std::string d) { scriptsDir_ = std::move(d); }            // --scripts <dir>
    void setSpawnTest(std::string cls) { spawnTestClass_ = std::move(cls); }      // --spawn-test <ClassName>
    void setPlayTest() { playTest_ = true; }                                       // --play-test
    void setProjectPath(std::string p) { projectPath_ = std::move(p); }          // <path>.ocproject
    void armBrowser(bool on) { browserActive_ = on; }   // shows the start screen

private:
    // Adopts the project the browser or command line loaded, and refreshes everything keyed to it.
    // Opening a project is the longest blocking thing the editor does after startup: it releases and
    // reloads every material and every mesh, cooks LOD pipelines, loads the start map and its
    // landscape, and starts the script host. All of it on the main thread, all of it between two
    // frames -- so the window stopped painting, Windows greyed it out and titled it "Not Responding",
    // and there was nothing on screen to say what was happening or that anything still was.
    //
    // THE SPLASH ALREADY SOLVED THIS ONCE, for engine startup, and its own header says why: "with a
    // static image there is no way to tell a slow start from a hung one. Naming the current stage
    // costs almost nothing and turns 'it froze' into 'it is compiling shaders'." The same window,
    // the same status line, reused for the same problem one layer up. setStatus repaints
    // SYNCHRONOUSLY and pump() drains the queue, which together are what keep it responsive while
    // the main window is busy.
    //
    // Scoped to the load: it appears when one starts and is destroyed when it ends, so nothing has
    // to remember to close it on an early return.
    struct LoadingScreen {
        Splash splash;
        bool on = false;
        Engine* borrowed = nullptr;   // non-null when reusing the engine's startup splash

        // `eng` is borrowed rather than owned when its startup splash is STILL UP. Opening a project
        // from the command line lands inside onInit, where that splash is showing; opening one from
        // the browser lands frames later, where it is long gone. Creating a second top-most window
        // over the first is the case this distinguishes.
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
            // No minimum visible time: a project that loads instantly should not be made to look
            // like it did not. Startup uses one because a splash that flashes reads as a glitch;
            // here the main window is already up behind it.
            // A borrowed splash belongs to Engine::run, which closes it when startup finishes.
            if (on) splash.close(0);
        }
    };

    void applyProject(Engine& e) {
        // Never in a capture or headless run: those have no splash at startup either, and a
        // top-most window would land in the middle of a screenshot.
        // e.window() IS the test: a headless run has no window, and a --frames capture opens one
        // unactivated (see Engine::run) but should not have a top-most splash land in a screenshot.
        // browserActive_ is false by then, so a capture run reaches applyProject only via an
        // explicit project path, which is exactly the case to stay silent for.
        // "\\splash.png", NOT "\splash.png" -- \s is not an escape MSVC knows, so it dropped the
        // backslash (warning C4129) and this asked for "...binsplash.png", which has never existed.
        // The loading screen has therefore been drawing with no image since it was written, falling
        // back silently because a missing splash is not treated as an error. The identical mistake
        // was in modules/runtime.game/src/GameApp.cpp's script-directory test, found in the same
        // sweep; both had been reported by the compiler on every build and never read.
        LoadingScreen loading(e, e.window() != nullptr && maxFrames_ == 0,
                              executableDir() + "\\splash.png");
        loading.stage("Opening project");

        project_ = browser_.project();
        editor::setActorEditorContentRoot(project_.contentDir());

        editor::setAnimEditorContentRoot(project_.contentDir());
        loading.stage("Applying project settings");
        applyProjectRenderSettings();
        startContentWatch();
        pendingUpgrade_ = editor::inspectProject(project_);
        upgradeAsked_ = false;
        upgradeStatus_.clear();
        if (!pendingUpgrade_.empty())
            AVER_INFO("[Editor] project '{}' predates {} of this editor's project files; offering to upgrade",
                      project_.name, pendingUpgrade_.fixes.size());
        if (e.window())
            e.window()->setTitle("Aver Engine \xE2\x80\x94 Editor \xE2\x80\x94 " + project_.name);
#if AVER_MODULE_PBR
        loading.stage("Loading materials");
        releaseProjectMaterials();
        rebuildContentIndex();
        loadProjectMaterials();
#endif
#if AVER_MODULE_SCENE
        loading.stage("Loading meshes");
        releaseProjectMeshes(e);
        loadProjectMeshes(e);
#endif
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
        loading.stage("Loading particle effects");
        loadProjectParticleEffects();
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
        // Moved here from onRender's per-frame call: pipeline/shader creation belongs at a project-
        // load/setup point, not mid-frame between beginFrame/endFrame, where the resource factory's
        // reentrancy with an in-flight command list was unverified. This runs exactly once regardless
        // (lodMeshPipelineTried_ latches), so calling it here instead of every onRender changes only
        // WHEN the one real attempt happens, not whether it does.
        loading.stage("Building virtualized-geometry pipelines");
        ensureLodMeshPipeline(e);
#endif
#if AVER_MODULE_SCENE
        loading.stage("Loading level");
        loadStartMap(e);
#endif
#if AVER_MODULE_SCRIPTING
        loading.stage("Starting scripts");
        if (scripts_.ready() && scriptsDir_.empty() && project_.valid()) {
            const std::string bin = editor::scriptsBinaryDir(project_);
            const i32 n = scripts_.loadScripts(bin);
            if (n > 0) AVER_INFO("[Scripting] {} project behaviour(s) live from {}", n, bin);
            else AVER_INFO("[Scripting] no built scripts in {} - use Tools > Reload Scripts", bin);
        }
        // GRAPH-AS-CLASS: registration, for composition-root parity with
        // modules/runtime.game/src/GameApp.cpp's own initScripting() call -- a seam wired in one root
        // and not the other is the exact defect aee2404 exists to fix. Independent of scriptsDir_/the
        // --scripts override just above: a graph class lives under the project's CONTENT directory,
        // not its compiled Scripts binaries, so it is declared regardless of which C# behaviour source
        // (if any) just loaded.
        if (scripts_.ready() && project_.valid()) {
            const i32 graphClasses = scripts_.declareGraphClasses(project_.contentDir());
            if (graphClasses > 0)
                AVER_INFO("[Graph] {} graph class(es) declared from '{}'", graphClasses, project_.contentDir());
        }
#endif
#if AVER_MODULE_FRAMEWORK
        // AFTER graph classes are declared (just above), and AFTER "Loading level" already collected
        // classPlacements_ (loadStartMap -> loadLevel) -- see that member's own comment for why this
        // is a separate, later call rather than inline in loadLevel. GATED ON scripts_.ready(), unlike
        // GameApp's own equivalent call site: a project opened from the COMMAND LINE reaches this
        // point BEFORE the scripting host ever bootstraps (see the scripts_.init(hd) call site's own
        // "GRAPH-AS-CLASS CATCH-UP" comment for why), and attempting a spawn before any class could
        // possibly be declared would only manufacture a "not declared" warning per placement for
        // nothing -- the catch-up call there is what actually spawns them, once scripting is real.
        //
        // The scripts_.ready() half is ITSELF under #if AVER_MODULE_SCRIPTING, separately from the
        // #if AVER_MODULE_FRAMEWORK this whole block already sits inside -- AVER_MODULE_FRAMEWORK's
        // only enforced dependency is AVER_MODULE_SCENE (see the root CMakeLists.txt), not
        // AVER_MODULE_SCRIPTING, so `scripts_` (a member that exists only under
        // AVER_MODULE_SCRIPTING) is not a name this block may touch unconditionally. A scripting-off
        // tree has no graph classes to have declared either way (declareGraphClasses is itself under
        // AVER_MODULE_SCRIPTING, just above), so the call below degrades to the same harmless
        // "not declared" warning path spawnClassPlacements already has for a class placement naming
        // an undeclared class -- matching GameApp's own equivalent call site's unconditional shape.
#if AVER_MODULE_SCRIPTING
        if (scripts_.ready())
#endif
            spawnClassPlacements();
#endif
    }

#if AVER_MODULE_LANDSCAPE
    // Loads one .ocland section and builds its quadtree. Pure CPU -- LandscapeRenderer's mesh cache
    // is created lazily by draw() the first time onRender() calls it, so this needs no device and may
    // run before one exists.
    //
    // `device` frees the section CURRENTLY resident (if any) through forgetAll before replacing it;
    // pass nullptr only when no device has been created yet, in which case there is nothing resident
    // to free either.
    // Returns whether a section is now resident, so a caller that has more to do to it (place it,
    // hand it to the chunk generator) can tell the difference between "loaded" and "there is no
    // terrain here", rather than reading landscapeLoaded_ back out and hoping it was this call that
    // set it.
    bool loadLandscape(rhi::IDevice* device, const std::string& path) {
        unloadLandscape(device);
        fmt::OcLandData data;
        std::string why;
        if (!fmt::loadOcLand(path, data, &why)) {
            AVER_WARN("[Landscape] could not load '{}': {}", path, why);
            return false;
        }
        landscape::LandscapeTree tree;
        if (!tree.build(data, landscape::kDefaultNodeQuads, &why)) {
            AVER_WARN("[Landscape] '{}' loaded but its quadtree would not build: {}", path, why);
            return false;
        }
        landscapeData_ = std::move(data);
        landscapeTree_ = std::move(tree);
        // The section is tile (0,0) of the ring now (see the member block's own comment) -- its outer
        // rim borders a procedural neighbour just like any ring tile's does, so it needs the same
        // generous, cross-tree-mismatch-proof floor. 2x the noise amplitude is the mathematical bound
        // on how much a ridged-fBm field can vary at all (see TerrainNoise.hpp), so this covers any
        // level a neighbour could have picked regardless of which it actually did.
        landscapeTree_.widenRimSkirts(2.0f * landscapeNoiseParams_.amplitudeCm);
        landscapeTree_.resetHysteresis();
        landscapeRenderer_ =
            std::make_unique<landscape::LandscapeRenderer>(kLandscapeMaxResidentNodesPerTile);
        landscapeLoaded_ = true;
        landscapePath_ = path;
        landscapeDirty_ = false;
        AVER_INFO("[Landscape] '{}' loaded: {} node(s) across {} level(s), {}x{} samples",
                  path, landscapeTree_.nodes().size(), landscapeTree_.levelCount(),
                  landscapeData_.sampleCount, landscapeData_.sampleCount);
        return true;
    }

    // Points a live chunk generator at the resident section, so scattered entities sit ON the
    // terrain instead of on the flat plane at the chunk's origin Z.
    //
    // CALLED FROM BOTH DIRECTIONS, because either can happen first: a level load can bring terrain in
    // while streaming is already running, and switching streaming on can find terrain already
    // resident. setChunkStreamingEnabled does the same wiring for the second case.
    //
    // THE LAMBDA CAPTURES `this`, NOT THE DATA. Sculpting mutates landscapeData_ in place, so a copy

    // safe because the generator lives in chunkWorld_, which this object owns and destroys.
    void applyLandscapeToStreaming() {
#if AVER_MODULE_SCENE
        // Nothing streaming yet: setChunkStreamingEnabled wires the source itself when it opens, so
        // the common order (level loads terrain, streaming switched on afterwards) needs nothing here.
        if (!chunkWorld_) return;
        // ChunkWorld exposes settings() as CONST ONLY -- there is no supported way to swap a live
        // generator's height source, and adding one to reach in from here would widen that module's
        // API for the editor's convenience. Restarting streaming re-opens through the same path that
        // already knows how to wire it, and re-generates only the chunks around the camera. Doing it
        // this way also drops every chunk generated against the OLD surface, which matters because
        // the region index's generatorVersion is carried but not yet compared -- a known gap, and
        // one this would otherwise fall straight into.
        AVER_INFO("[ChunkWorld] terrain changed under a live stream -- restarting it so scatter "
                  "re-generates against the new surface");
        setChunkStreamingEnabled(false);
        setChunkStreamingEnabled(true);
#endif
    }

    // Gives the resident section a static collision body, so things can stand on the terrain rather
    // than only look at it.
    //
    // REBUILT WHOLE, not patched. Jolt's heightfield shape is immutable once created, and
    // PhysicsBridge converts the entire section (transposing into its row order) in one pass; there
    // is no partial update to reach for. That is why this is called at LOAD and at SAVE rather than
    // per brush stroke -- a stroke would pay for the whole conversion, several times a second.
    //
    // THE CONSEQUENCE, stated rather than hidden: between sculpting and saving, what you see and
    // what you collide with disagree. The visual mesh and the scatter's height source both read
    // landscapeData_ live; the body is a snapshot.
    void rebuildLandscapeCollision() {
#if AVER_MODULE_PHYSICS
        if (landscapeBody_ >= 0) { aver_phys_remove_body(landscapeBody_); landscapeBody_ = -1; }
        if (!landscapeLoaded_ || !aver_phys_ready()) return;
        landscape::PhysicsHeightfield hf;
        if (!landscape::toPhysicsHeightfield(landscapeData_, hf)) {
            AVER_WARN("[Landscape] section is not internally consistent; no collision built");
            return;
        }
        landscapeBody_ = aver_phys_add_heightfield(hf.samples.data(), static_cast<i32>(hf.sampleCount),
                                                   hf.spacingCm, hf.cornerCm[0], hf.cornerCm[1],
                                                   hf.cornerCm[2]);
        // Deliberately NOT stamped with aver_phys_set_entity: terrain has no owning scene entity in
        // this engine at all, not merely one this call site forgot to look up. A ray landing on it
        // is a genuine "hit true, entity 0" -- something WAS hit, nothing owns it -- not a bug.
        if (landscapeBody_ >= 0)
            AVER_INFO("[Landscape] collision body #{} built ({}x{} samples)", landscapeBody_,
                      hf.sampleCount, hf.sampleCount);
        else
            AVER_WARN("[Landscape] physics refused the heightfield; terrain has no collision");
#endif
    }

    // Frees the resident section's meshes (when a device exists to free them through) and drops it,
    // AND every ring tile around it -- they are tile (0,0)'s neighbours and outlive their reason to
    // exist the moment (0,0) does.
    void unloadLandscape(rhi::IDevice* device) {
        if (landscapeRenderer_ && device) landscapeRenderer_->forgetAll(*device);
        landscapeRenderer_.reset();
#if AVER_MODULE_PHYSICS
        if (landscapeBody_ >= 0) { aver_phys_remove_body(landscapeBody_); landscapeBody_ = -1; }
#endif
        landscapeLoaded_ = false;
        landscapePath_.clear();
        landscapeData_ = fmt::OcLandData{};
        landscapeDirty_ = false;
        sculpting_ = false;
        sculptCursorValid_ = false;
        for (auto& kv : landscapeRingTiles_)
            if (kv.second.renderer && device) kv.second.renderer->forgetAll(*device);
        landscapeRingTiles_.clear();
        landscapeLastCameraTileValid_ = false;
    }

    // Writes the in-memory section back to the path it was loaded from -- the --landscape override
    // or the levelname.ocland convention loadLandscapeForLevel resolved (see landscapePath_'s own
    // comment). A sculpt is fully functional in memory without this; it is the one place edits
    // actually reach disk. Silent no-op if there is nothing loaded or nowhere to write it, matching
    // the Save Landscape menu item's own BeginDisabled guard.
    void saveLandscape() {
        if (!landscapeLoaded_ || landscapePath_.empty()) return;
        std::string why;
        if (fmt::saveOcLand(landscapePath_, landscapeData_, &why)) {
            landscapeDirty_ = false;
            AVER_INFO("[Landscape] saved '{}'", landscapePath_);
            // Saving is the natural commit point for a sculpt, so it is where the collision snapshot
            // catches up with the heights -- see rebuildLandscapeCollision on why this is not done
            // per stroke.
            rebuildLandscapeCollision();
        } else {
            AVER_ERROR("[Landscape] could not save '{}': {}", landscapePath_, why);
        }
    }

    // Resolves which .ocland a just-loaded level should draw: the --landscape override if one was
    // given, else <levelPath with its extension swapped to .ocland> -- "named by the level". Silent
    // when neither exists: most levels have no terrain yet, and that must not warn on every load.
    // Resolves which .ocland a level is standing on, in this order:
    //   1. --landscape <path>, an explicit override that always wins
    //   2. the level's own LANDSCAPE record, resolved against the project's Content
    //   3. the levelname.ocland convention
    //
    // (2) IS WHY THE FORMAT RECORD EXISTS. The format learned to carry a LANDSCAPE record and the
    // editor learned to draw an .ocland in the same change, and the two halves were never joined --
    // so a level could declare its terrain and the editor would ignore the declaration and go
    // looking for a filename. The convention stays as the last resort because it costs nothing and
    // levels authored before the record exists still open.
    //
    // `at` OVERRIDES THE SECTION'S OWN originCm, and doing it here -- in the data, once -- is what
    // makes every consumer agree without being told about placement separately. The renderer draws
    // with an identity world matrix, the sculpt raycast walks the same grid, the height source
    // samples it, and the physics bridge converts it; all four read originCm, so moving the section
    // is one assignment rather than four transforms that could disagree.
    // Pushes the level's LANDSCAPE material into one landscape renderer, as an opaque binding.
    //
    // THE HOST DOES THE RESOLVING, which is the whole reason LandscapeRenderer::setSurfaceBinding
    // takes bytes and a handle instead of a pbr:: type. Aver.Landscape.Renderer links Core, RHI and
    // Aver.Landscape only; the material system lives here, in the composition root, exactly as the
    // concrete Voxi and AverSR types do.
    void applyLandscapeSurface(landscape::LandscapeRenderer& r) {
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        if (landscapeMaterial_.empty()) return;
        pbr::MaterialSystem& ms = voxiRenderer_.materials();
        if (!ms.ready()) return;
        const pbr::MaterialHandle h = materialForSurface(landscapeMaterial_);
        if (!h) return;
        const pbr::MaterialConstants& mc = ms.constants(h);
        r.setSurfaceBinding(ms.bindingSet(h), &mc, sizeof(pbr::MaterialConstants));

        // AND THE TEXTURE SCALE, which is the other half of "apply the material" and was missing.
        // LandscapeRenderer::draw() takes uvTilingCm and bakes it into the chunk mesh's UVs; neither
        // call site passed one, so every landscape in this editor drew at the parameter's compiled-in
        // 1000cm default -- ten-metre tiles. M_forest_leaves_02 authors `PARAM uvTiling 150`, and its
        // own comment says why: the Poly Haven source is a two-metre texture. Ten-metre tiles stretch
        // it 6.7x past the scale it was made for, which is exactly the pale, washed-out, low-frequency
        // ground the demo captures show.
        //
        // WHY THIS READS uvTilesPerCm RATHER THAN THE DESC. MaterialSystem exposes constants(), not
        // the MaterialDesc, and the packed block already carries the reciprocal -- so this needs no
        // new accessor and no new coupling. The host resolving it here is the shape this function's
        // own header comment describes: Aver.Landscape.Renderer never learns what a pbr:: material is.
        //
        // A NOTE ON uvTiling'S DOCUMENTED SCOPE. MaterialDesc calls it "read only under WorldAligned",
        // and that is true OF THE SHADER: averSurfaceUV only consults gUvTilesPerCm when the material
        // sets worlduv=1, which this one does not. It is not a contradiction. Both uses mean the same
        // thing -- world centimetres per tile -- and differ only in who applies it: the shader, by
        // projecting; or the mesh builder, by baking it into UVs. The landscape is the second kind.
        //
        // AND IT MUST BE THE REAL MATERIAL'S NUMBER, NOT THE FALLBACK'S. MaterialSystem::constants()
        // hands back fallbackConstants_ for any handle MaterialLibrary does not yet consider valid,
        // and materialForSurface() can create a material in the library on the very frame this runs
        // -- before MaterialSystem::update() has drained consumeDirty() and built an entry for it.
        // The fallback is packMaterial(MaterialDesc{}), whose uvTiling is 200, so taking it silently
        // would latch the landscape at 200cm and look like a plausible number rather than a bug.
        // Ask the library the same question constants() asks, and only believe the answer when it
        // says yes; the caller retries until then.
        if (pbr::MaterialLibrary::get().valid(h) && mc.uvTilesPerCm > 0.0f) {
            landscapeUvTilingCm_ = 1.0f / mc.uvTilesPerCm;
            landscapeUvTilingResolved_ = true;
        }
#else
        (void)r;
#endif
    }

    // Re-applies it to the authored section and every resident ring tile at once. Called after a
    // level load, and after the material system becomes ready -- whichever happens second is the one
    // that actually binds anything, and neither is reliably first.
    void applyLandscapeSurfaceToAll(rhi::IDevice* device = nullptr) {
        const f32 wasTiling = landscapeUvTilingCm_;
        if (landscapeRenderer_) applyLandscapeSurface(*landscapeRenderer_);
        for (auto& kv : landscapeRingTiles_)
            if (kv.second.renderer) applyLandscapeSurface(*kv.second.renderer);

        // THE CACHED MESHES CARRY THE OLD SCALE, so the eviction belongs here rather than inside
        // applyLandscapeSurface: that runs once per renderer and updates the shared member on its
        // first call, so a per-renderer comparison would evict the section and silently leave every
        // ring tile holding UVs built at the previous tiling. Compare once, around the whole sweep.
        //
        // buildChunkMesh bakes uvTilingCm into a node's UVs and draw() caches the result, so nothing
        // already resident picks up a change on its own. Fires at most once per level -- the frame the
        // material system finally comes up, when the landscape had already loaded and drawn flat --
        // and the nodes rebuild lazily on the next draw, exactly as they already do after a sculpt.
        // THIS BLOCK IS NORMALLY SILENT, AND THAT IS NOT A SIGN IT DID NOTHING. The first apply
        // happens from loadLandscapeForLevel, before a single chunk mesh has been built, so there is
        // nothing resident to evict and nothing to report -- the tiling simply takes effect on every
        // mesh built afterwards. Verified by instrumenting draw() directly: it receives 150, from
        // M_forest_leaves_02's own PARAM uvTiling, rather than the 1000cm parameter default. The
        // absence of this log was briefly mistaken for the fix not working; it is the opposite.
        if (device && landscapeUvTilingCm_ != wasTiling) {
            if (landscapeRenderer_) landscapeRenderer_->forgetAll(*device);
            for (auto& kv : landscapeRingTiles_)
                if (kv.second.renderer) kv.second.renderer->forgetAll(*device);
            AVER_INFO("[Landscape] texture tiling {:.0f}cm per tile, from material '{}' "
                      "(was {:.0f}); resident nodes dropped to rebuild",
                      landscapeUvTilingCm_, landscapeMaterial_, wasTiling);
        }
    }

    void loadLandscapeForLevel(rhi::IDevice* device, const std::string& levelPath,
                               const fmt::OcWorldData& w) {
        std::string path = landscapeCliOverride_;
        bool haveAt = false;
        f64 at[3] = {0, 0, 0};

        if (path.empty() && !w.landscapes.empty()) {
            const fmt::OcLandscapePlacement& lp = w.landscapes.front();
            if (w.landscapes.size() > 1)
                AVER_WARN("[Landscape] level declares {} LANDSCAPE sections; the editor holds one and "
                          "is using '{}'. Tiling several sections is not implemented.",
                          w.landscapes.size(), lp.name.empty() ? lp.section : lp.name);
            // Taken even when the section path below fails: the material is a property of the
            // level's terrain, not of which file the heights came from, and the .ocland fallback
            // convention still wants it.
            landscapeMaterial_ = lp.material;
            if (!lp.section.empty()) {
                const std::string content = project_.contentDir();
                path = content.empty() ? lp.section : content + "\\" + lp.section;
                std::error_code ec;
                if (!std::filesystem::exists(path, ec)) {
                    AVER_WARN("[Landscape] level's LANDSCAPE section '{}' does not exist at '{}' -- "
                              "falling back to the levelname.ocland convention", lp.section, path);
                    path.clear();
                } else {
                    at[0] = lp.x; at[1] = lp.y; at[2] = lp.z;
                    haveAt = true;
                }
            }
        }

        const bool explicitPath = !path.empty();
        if (!explicitPath) {
            std::filesystem::path p(levelPath);
            p.replace_extension(".ocland");
            path = p.string();
        }
        std::error_code ec;
        if (!explicitPath && !std::filesystem::exists(path, ec)) return;
        if (!loadLandscape(device, path)) return;

        if (haveAt) {
            landscapeData_.originCm[0] = static_cast<f32>(at[0]);
            landscapeData_.originCm[1] = static_cast<f32>(at[1]);
            landscapeData_.originCm[2] = static_cast<f32>(at[2]);
            // The tree caches node centres and bounds derived from originCm, so it has to be rebuilt
            // rather than nudged -- otherwise LOD selection and frustum culling would run against
            // where the section used to be.
            std::string why;
            if (landscapeTree_.build(landscapeData_, landscapeTree_.nodeQuads(), &why)) {
                landscapeTree_.widenRimSkirts(2.0f * landscapeNoiseParams_.amplitudeCm);
                landscapeTree_.resetHysteresis();
                if (landscapeRenderer_ && device) landscapeRenderer_->forgetAll(*device);
                // The section moved, so its ring-tile grid (centred on ITS centre) moved too --
                // whatever was resident was built against the old placement and no longer borders it
                // correctly. Simplest correct fix: drop the ring and let updateLandscapeRingTiles()
                // resynthesize around wherever the camera is next frame.
                for (auto& kv : landscapeRingTiles_)
                    if (kv.second.renderer && device) kv.second.renderer->forgetAll(*device);
                landscapeRingTiles_.clear();
                landscapeLastCameraTileValid_ = false;
                AVER_INFO("[Landscape] placed at ({:.0f}, {:.0f}, {:.0f}) by the level's LANDSCAPE record",
                          at[0], at[1], at[2]);
            } else {
                AVER_ERROR("[Landscape] could not rebuild after placement: {}", why);
            }
        }
        rebuildLandscapeCollision();
        applyLandscapeToStreaming();
        applyLandscapeSurfaceToAll(device);
    }

    // Keeps a small window of PROCEDURAL tiles resident around (cameraXCm, cameraYCm), so the terrain
    // drawn extends past the authored section's own rim instead of stopping dead at it. Cheap to call
    // every frame -- it does real work only the frame the camera's OWN tile coordinate changes, which
    // for a 30000cm-ish tile is far less often than once a frame.
    //
    // Tile (0, 0) -- the authored section -- is never touched here; it is loaded, sculpted and saved
    // exactly as it always was. This only manages the RING around it.
    void updateLandscapeRingTiles(rhi::IDevice* device, f32 cameraXCm, f32 cameraYCm) {
        if (!landscapeLoaded_) return;
        const f32 tileSizeCm = landscapeData_.extentCm();
        if (!(tileSizeCm > 0.0f)) return;
        const f32 centreX = landscapeData_.originCm[0] + tileSizeCm * 0.5f;
        const f32 centreY = landscapeData_.originCm[1] + tileSizeCm * 0.5f;

        const landscape::TileCoord camTile =
            landscape::tileAt(cameraXCm, cameraYCm, centreX, centreY, tileSizeCm);
        if (landscapeLastCameraTileValid_ && camTile == landscapeLastCameraTile_) return;
        landscapeLastCameraTile_ = camTile;
        landscapeLastCameraTileValid_ = true;

        // Which coordinates should be resident now -- a (2R+1)x(2R+1) window around the camera's own
        // tile, minus (0,0) itself (that is the home tile above, not a ring tile).
        std::vector<landscape::TileCoord> want;
        want.reserve(kLandscapeMaxSectionsResident);
        for (i32 dy = -kLandscapeRingRadius; dy <= kLandscapeRingRadius; ++dy)
            for (i32 dx = -kLandscapeRingRadius; dx <= kLandscapeRingRadius; ++dx) {
                const landscape::TileCoord t{camTile.tx + dx, camTile.ty + dy};
                if (t.tx == 0 && t.ty == 0) continue;
                want.push_back(t);
            }

        // Evict whatever is resident but no longer wanted.
        for (auto it = landscapeRingTiles_.begin(); it != landscapeRingTiles_.end(); ) {
            const bool stillWanted = std::find(want.begin(), want.end(), it->first) != want.end();
            if (!stillWanted) {
                if (it->second.renderer && device) it->second.renderer->forgetAll(*device);
                it = landscapeRingTiles_.erase(it);
            } else {
                ++it;
            }
        }

        // Synthesize and build whatever is wanted but not yet resident. Same sample count as the
        // authored section -- it already validated against LandscapeTree::build's tiling rule, so a
        // ring tile built the same way is guaranteed to validate too.
        for (const landscape::TileCoord& t : want) {
            if (landscapeRingTiles_.find(t) != landscapeRingTiles_.end()) continue;
            LandscapeRingTile tile;
            if (!landscape::synthesizeTerrainTile(t, centreX, centreY, tileSizeCm,
                                                  landscapeData_.sampleCount, landscapeNoiseParams_,
                                                  tile.data)) {
                AVER_WARN("[Landscape] could not synthesize ring tile ({}, {})", t.tx, t.ty);
                continue;
            }
            std::string why;
            if (!tile.tree.build(tile.data, landscapeTree_.nodeQuads(), &why)) {
                AVER_WARN("[Landscape] ring tile ({}, {}) quadtree would not build: {}", t.tx, t.ty, why);
                continue;
            }
            // Every rim of a ring tile borders SOMETHING -- the home tile, or another ring tile --
            // never open air, so all four get the same generous floor the home tile's outer rim got in
            // loadLandscape(). See LandscapeTree::widenRimSkirts's own comment for why an inner-LOD
            // skirt formula cannot cover a cross-tree neighbour.
            tile.tree.widenRimSkirts(2.0f * landscapeNoiseParams_.amplitudeCm);
            tile.tree.resetHysteresis();
            tile.renderer =
                std::make_unique<landscape::LandscapeRenderer>(kLandscapeMaxResidentNodesPerTile);
            // A tile born mid-session has to be told the surface too, or the ring renders untextured
            // around a textured home section -- a seam that moves with the camera.
            applyLandscapeSurface(*tile.renderer);
            landscapeRingTiles_.emplace(t, std::move(tile));
        }

        AVER_INFO("[Landscape] ring around tile ({}, {}): {} tile(s) resident, {} draws/tile, "
                  "{} resident-node cap/tile", camTile.tx, camTile.ty, landscapeRingTiles_.size(),
                  kLandscapeMaxDrawsPerTile, kLandscapeMaxResidentNodesPerTile);
    }
#endif

    // Tells the formats layer where averdesign.exe is installed. Not scripting-specific -- it points
    // at the Roslyn build tool (gated on AVER_HAVE_ROSLYN just below) and is called unguarded from
    // onInit(). This, and everything down to reloadScripts(), used to sit inside one
    // AVER_MODULE_SCRIPTING block, but only resolveScriptsDir() and reloadScripts() actually touch
    // scripts_/ScriptHost -- the rest (render-settings, project-manifest, HUD-preview and
    // content-watch code) is called from sites that are themselves unguarded. With SCRIPTING off the
    // declarations vanished while those callers remained, so the guard is narrowed to just the two
    // functions that need it rather than widening every call site to match.
    void locateAverDesign() const {
#if AVER_HAVE_ROSLYN
        fmt::setAverDesignPath(executableDir() + "/Tools/averdesign.exe");
#endif
    }

    // Returns the directory the CLR host loads user assemblies from: --scripts, else the project's
    // Binaries\Scripts, else <exe>\Scripts.
#if AVER_MODULE_SCRIPTING
    std::string resolveScriptsDir() const {
        if (scriptsDir_.empty())
            return project_.valid() ? editor::scriptsBinaryDir(project_) : executableDir() + "\\Scripts";
        const std::string& sd = scriptsDir_;
        const bool absolute = sd.size() > 1 && (sd[1] == ':' || sd[0] == '\\' || sd[0] == '/');
        return absolute ? sd : executableDir() + "\\" + sd;
    }
#endif

#if AVER_MODULE_VOXI
    // Pushes the manifest's render settings into Voxi. Defers until the device info is known.
    void applyProjectRenderSettings() {
        if (!project_.valid() || !project_.hasRenderSettings()) return;
        if (!voxiAttached_) { projectRenderPending_ = true; return; }
        projectRenderPending_ = false;

        voxi::Renderer& vx = voxi::Renderer::get();
        voxi::Settings s = vx.settings();
        if (project_.giQuality       >= 0)    s.globalIllumination = static_cast<voxi::Quality>(project_.giQuality);
        if (project_.rayTracing      >= 0)    s.rayTracing         = static_cast<voxi::Quality>(project_.rayTracing);
        if (project_.pathTracing     >= 0)    s.pathTracing        = static_cast<voxi::Quality>(project_.pathTracing);
        if (project_.voxelResolution >  0)    s.voxelResolution    = static_cast<u32>(project_.voxelResolution);
        if (project_.giIntensity     >= 0.0f) s.giIntensity        = project_.giIntensity;
        if (project_.giMaxDistance   >= 0.0f) s.giMaxDistance      = project_.giMaxDistance;
        if (project_.rtShadowRays       >= 0) s.rtShadowRays       = static_cast<u32>(project_.rtShadowRays);
        if (project_.rtPixelsPerRayTile >= 0) s.rtPixelsPerRayTile = static_cast<u32>(project_.rtPixelsPerRayTile);
        if (project_.rtShadowDenoise    >= 0) s.rtShadowDenoise    = static_cast<u32>(project_.rtShadowDenoise);
        vx.setSettings(s);   // clamps to this device; the manifest keeps what was asked for
        voxiRenderer_.setSettings(vx.settings());
        // A manifest that names Path Tracing explicitly should actually (de)register PtSceneView at
        // load, the same way every other feature above already takes effect just by existing in
        // voxiRenderer_'s own per-frame settings read: PathTracing needs an explicit register/
        // unregister step instead (see syncPtSceneView(), called later this frame from onInit(), or
        // on the next onUpdate() for a project opened mid-session via this same function at line
        // ~4615). Guarded on project_.pathTracing >= 0 -- i.e. RENDER.PATHTRACING is actually PRESENT
        // in the manifest (see OcProject.hpp) -- so a project that states nothing about it never
        // silently turns off a view --pt-scene, a toggle-test flag, or the settings combo itself
        // already asked for this session; reading the CLAMPED vx.settings() (not local `s`) means a
        // manifest requesting PT on hardware that cannot run it does not try to register it anyway.
        if (project_.pathTracing >= 0)
            ptSceneViewWantEnabled_ = (vx.settings().pathTracing != voxi::Quality::Off);
        AVER_INFO("[Project] applied render settings from {}", project_.manifestPath);
    }

    // Copies the controls' requested values into the manifest struct, before the renderer clamps them.
    void captureRenderSettingsFromUi(const voxi::Settings& requested) {
        project_.giQuality       = static_cast<int>(requested.globalIllumination);
        project_.rayTracing      = static_cast<int>(requested.rayTracing);
        project_.pathTracing     = static_cast<int>(requested.pathTracing);
        // If the GI tier just changed here and voxelResolution was NOT independently touched in
        // this same edit, leave the manifest's voxelResolution unset (-1) instead of baking in the
        // number it held under the OLD tier: vx.setSettings (called right after this) is about to
        // derive the new tier's rung from the identical signal (see Renderer::setSettings), and
        // pinning the stale pre-derivation value here would silently stop this project from ever
        // letting its GI quality drive the grid again on a later load.
        const voxi::Settings& live = voxi::Renderer::get().settings();
        const bool tierOnlyChange = requested.globalIllumination != live.globalIllumination
                                  && requested.voxelResolution == live.voxelResolution;
        project_.voxelResolution = tierOnlyChange ? -1 : static_cast<int>(requested.voxelResolution);
        project_.giIntensity     = requested.giIntensity;
        project_.giMaxDistance   = requested.giMaxDistance;
        project_.rtShadowRays       = static_cast<int>(requested.rtShadowRays);
        project_.rtPixelsPerRayTile = static_cast<int>(requested.rtPixelsPerRayTile);
        project_.rtShadowDenoise    = static_cast<int>(requested.rtShadowDenoise);
        projectDirty_ = true;
    }
#else
    void applyProjectRenderSettings() {}
#endif

    // Fills any unstated render-settings key with the engine's declared default, then writes the
    // manifest. --save-project.
    void seedAndSaveProject() {
#if AVER_MODULE_VOXI
        const voxi::Settings d{};   // as declared, never as clamped
        if (project_.giQuality       < 0)    project_.giQuality       = static_cast<int>(d.globalIllumination);
        if (project_.rayTracing      < 0)    project_.rayTracing      = static_cast<int>(d.rayTracing);
        if (project_.pathTracing     < 0)    project_.pathTracing     = static_cast<int>(d.pathTracing);
        // Derived from the (now-seeded) GI tier, not from d.voxelResolution's raw struct default --
        // otherwise a manifest that only states giQuality would bake in Medium's 128 regardless of
        // the tier, and never let that tier drive the grid on a later load (see Renderer::setSettings).
        if (project_.voxelResolution <= 0)
            project_.voxelResolution = static_cast<int>(
                voxi::Renderer::voxelResolutionForQuality(static_cast<voxi::Quality>(project_.giQuality)));
        if (project_.giIntensity     < 0.0f) project_.giIntensity     = d.giIntensity;
        if (project_.giMaxDistance   < 0.0f) project_.giMaxDistance   = d.giMaxDistance;
        if (project_.rtShadowRays       < 0) project_.rtShadowRays       = static_cast<int>(d.rtShadowRays);
        if (project_.rtPixelsPerRayTile < 0) project_.rtPixelsPerRayTile = static_cast<int>(d.rtPixelsPerRayTile);
        if (project_.rtShadowDenoise    < 0) project_.rtShadowDenoise    = static_cast<int>(d.rtShadowDenoise);
#endif
        std::string why;
        if (saveProjectManifest(&why)) AVER_INFO("[Project] --save-project wrote the manifest");
        else                           AVER_WARN("[Project] --save-project failed: {}", why);
    }

    // Writes the project manifest to disk. Returns false and fills why on failure.
    bool saveProjectManifest(std::string* why) {
        if (!project_.valid()) { if (why) *why = "no project is open"; return false; }
        std::string existing;
        readFileText(project_.manifestPath, existing);
        const std::string out = fmt::writeOcproject(project_, existing);
        if (!writeFileText(project_.manifestPath, out)) {
            if (why) *why = "could not write " + project_.manifestPath;
            return false;
        }
        projectDirty_ = false;
        AVER_INFO("[Project] wrote {}", project_.manifestPath);
        return true;
    }
    bool projectDirty_ = false;
    bool projectRenderPending_ = false;   // manifest read before the device attached
    std::string projectSaveStatus_;

    // True when the HUD preview may draw: a tab published a rect and no session is playing.
    bool hudPreviewActive() const {
#if AVER_MODULE_SCRIPTING && AVER_MODULE_FRAMEWORK
        return hudPreviewIndex_ >= 0 && hudRectW_ > 1.0f &&
               aver_fw_play_state() != AVER_FW_PLAY_PLAYING;
#elif AVER_MODULE_SCRIPTING
        return hudPreviewIndex_ >= 0 && hudRectW_ > 1.0f;
#else
        return false;
#endif
    }
    // Published by the HUD tab each frame it draws; cleared when it does not.
    void setHudPreview(int index, f32 x, f32 y, f32 w, f32 h) {
        hudPreviewIndex_ = index; hudRectX_ = x; hudRectY_ = y; hudRectW_ = w; hudRectH_ = h;
    }
    int hudPreviewIndex_ = -1;
    int hudTest_ = -1;
    bool saveProject_ = false;
    std::string importSrc_, importDst_;
    bool importDone_ = false;
    bool saveProjectDone_ = false;
    bool hudTestReported_ = false;
    f32 hudRectX_ = 0, hudRectY_ = 0, hudRectW_ = 0, hudRectH_ = 0;

    // Starts watching the project's content root, recursively, for changes made outside the editor.
    void startContentWatch() {
        contentWatch_.stop();
        if (!project_.valid()) return;
        const std::string root = project_.contentDir();
        if (root.empty()) return;
        if (contentWatch_.start(root, /*recursive=*/true))
            AVER_INFO("[Editor] watching '{}' for changes made outside this editor", root);
    }

    // Drains the watcher once a frame and tells the asset editors what changed.
    void pumpContentWatch() {
        if (!contentWatch_.watching()) return;
        watchEvents_.clear();
        if (contentWatch_.poll(watchEvents_)) {
            AVER_WARN("[Editor] the watcher lost records; every open editor is being told to re-read");
            assetEditors_.notifyWatchLost();
// BOTH modules, not just PBR. materials() is the PBR system, but it is reached THROUGH
// voxiRenderer_, which is declared under AVER_MODULE_VOXI. Guarding on PBR alone compiles the call
// in a VOXI=OFF + PBR=ON build where the member does not exist -- which is exactly the
// configuration that has been failing to build at head, unnoticed, because the module matrix
// script that would have caught it could not run (see scripts/module-matrix.ps1's Get-Cached).
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
            // Records were lost, so a texture may have arrived unseen. Forgetting the failed
            // resolves is cheap and the alternative is a material stuck on its fallback forever.
            voxiRenderer_.materials().forgetFailedResolves();
#endif
            if (autoCompile_) scheduleAutoCompile("the watcher lost records");
            return;
        }
        bool sawImage = false;
        for (const FileEvent& ev : watchEvents_) {
            if (ev.kind == FileChange::Deleted) continue;
            const std::string full = (std::filesystem::path(contentWatch_.root()) / ev.path).string();
            if (assetEditors_.notifyFileChanged(full))
                AVER_TRACE("[Editor] '{}' changed on disk; its tab was told", ev.path);
            if (autoCompile_ && isScriptSource(ev.path)) scheduleAutoCompile(ev.path);
            if (isTextureSource(ev.path)) sawImage = true;
        }
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        // AN IMAGE APPEARED OR CHANGED UNDER THE CONTENT ROOT, which is the one moment a texture
        // that failed to resolve might now succeed. The material system remembers failures so a
        // material naming a missing file does not re-hit the filesystem every drain; without this
        // call that memory outlives the fix, and dropping a PNG into the project would do nothing
        // until the editor restarted.
        if (sawImage) voxiRenderer_.materials().forgetFailedResolves();
#else
        (void)sawImage;
#endif
        serviceAutoCompile();
    }

    // True for an image the texture loader can actually decode. Kept in step with
    // modules/platform/src/Image.cpp, which is stb_image: a format listed here that stb cannot read
    // costs one wasted retry, and one it CAN read that is missing here never triggers a retry.
    static bool isTextureSource(const std::string& rel) {
        const usize dot = rel.find_last_of('.');
        if (dot == std::string::npos) return false;
        std::string ext = rel.substr(dot + 1);
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "tga" || ext == "bmp" ||
               ext == "psd" || ext == "gif" || ext == "hdr" || ext == "pic" || ext == "ppm" ||
               ext == "pgm" || ext == "octex";
    }

    // True for a hand-written .cs under the content root. Excludes bin/ and obj/ path segments.
    static bool isScriptSource(const std::string& rel) {
        if (rel.size() < 4 || rel.compare(rel.size() - 3, 3, ".cs") != 0) return false;
        for (usize seg = 0; seg < rel.size(); ) {
            const usize slash = rel.find('/', seg);
            const usize len = (slash == std::string::npos ? rel.size() : slash) - seg;
            const std::string_view part(rel.data() + seg, len);
            if (part == "bin" || part == "obj") return false;
            if (slash == std::string::npos) break;
            seg = slash + 1;
        }
        return true;
    }

    // Pushes the auto-compile deadline out, so a burst of changes produces one build.
    void scheduleAutoCompile(const std::string& why) {
        autoCompileDue_ = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kAutoCompileQuietMs);
        if (autoCompileReason_.empty()) autoCompileReason_ = why;
        ++autoCompilePending_;
    }

    // Starts the queued auto-compile once its deadline passes and no build is running.
    void serviceAutoCompile() {
        if (autoCompilePending_ == 0) return;
        if (std::chrono::steady_clock::now() < autoCompileDue_) return;
        if (tools_.compiling()) {
            autoCompileDue_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
            return;
        }
        if (!project_.valid()) { autoCompilePending_ = 0; autoCompileReason_.clear(); return; }
        AVER_INFO("[Editor] auto-compile: {} script change(s) settled (first was '{}')",
                  autoCompilePending_, autoCompileReason_);
        autoCompilePending_ = 0;
        autoCompileReason_.clear();
        tools_.triggerToolbarCompile(project_);
    }

    DirectoryWatcher contentWatch_;
    std::vector<FileEvent> watchEvents_;

    bool autoCompile_ = false;
    int focusLevelAt_ = 0;
    // Frames left before chunk streaming auto-enables; 0 = off. ON BY DEFAULT, at the same 5-frame
    // delay a bare --chunk-stream asks for.
    //
    // IT USED TO DEFAULT TO OFF, and that made the editor open on an empty world: without streaming
    // nothing runs the scatter generator, so a level with a 33-species palette showed its terrain,
    // its 14 hand-placed pines, and nothing else. The level looked broken and the flag that fixed it
    // was undiscoverable -- opt-in is right for a feature that costs something a user might not want,
    // and wrong for the one that puts the world in the world.
    //
    // The delay is not cosmetic: streaming keys off the camera, and frameCameraOn moves it on level
    // load, so enabling on frame 0 would stream a ring around wherever the camera happened to start
    // and immediately evict it. --no-chunk-stream turns it off for a static scene.
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
    bool reloadScripts(const std::string& binDir, std::string* status) {
        if (!scripts_.ready()) {
            if (status) *status = "The scripting host is not running: " + scripts_.declineReason();
            return false;
        }
        const bool collected = scripts_.unloadScripts();
        const i32 n = scripts_.loadScripts(binDir);
        if (n < 0) {
            if (status) *status = "The scripting host refused the load.";
            return false;
        }
        if (status) {
            *status = std::to_string(n) + " behaviour(s) live from " + binDir +
                      (collected ? "" : " (the previous load context is still finalising)");
        }
        editor::notifyActorEditorsScriptsReloaded();
        return true;
    }
#endif

#if AVER_MODULE_FRAMEWORK
    // Spawns one instance of the --spawn-test class, then destroys it a few frames later.
    void maybeSpawnTestActor() {
        if (spawnTestClass_.empty()) return;

        if (!spawnTestDone_) {
            spawnTestDone_ = true;
            const int32_t c = aver_fw_class_find(spawnTestClass_.c_str());
            if (c == 0) {
                AVER_WARN("[spawn-test] no class named '{}' is declared - is the script assembly loaded? "
                          "(pass --scripts <dir> pointing at the built actor assembly)", spawnTestClass_);
                return;
            }
            spawnTestEntity_ = aver_fw_spawn(c, "spawn-test-instance", nullptr, nullptr, nullptr);
            if (spawnTestEntity_ == 0)
                AVER_WARN("[spawn-test] class '{}' failed to spawn", spawnTestClass_);
            else
                AVER_INFO("[spawn-test] spawned '{}' as entity {} - watch for its OnBeginPlay/OnTick lines",
                          spawnTestClass_, spawnTestEntity_);
            return;
        }

        if (spawnTestEntity_ != 0 && ++spawnTestFrames_ == 3) {
            AVER_INFO("[spawn-test] destroying entity {} - watch for its OnEndPlay line", spawnTestEntity_);
            aver_fw_destroy(spawnTestEntity_);
            spawnTestEntity_ = 0;
        }
    }

    // Starts a play session: finds the user GameMode and optional GameInstance and begins play.
    //
    // WITH NO GAMEMODE, falls back to the drone rather than doing nothing. Pressing Play used to
    // log a warning and return, which is a poor answer in a project that has content but no scripts
    // yet -- the button appears to be broken. A level with nothing attached now gets the
    // graph-driven drone as its default configuration: something moves, the camera has something to
    // follow, and chunk streaming has a viewer that is actually inside the generated band.
    //
    // A REAL GameMode always wins. This is only ever reached when the project declares none, so a
    // game that defines its own play behaviour never sees the drone.
    void startPlay() {
        const int32_t gm = aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_MODE);
        if (gm == 0) {
            // scene:: is safe to reach here without a further guard: the root CMakeLists forces
            // AVER_MODULE_FRAMEWORK off when AVER_MODULE_SCENE is off (CMakeLists.txt:169-171), and
            // this whole function is inside #if AVER_MODULE_FRAMEWORK.
            if (droneEntity_ == scene::kInvalidEntity) {
                setDroneEnabled(true);
                // setDroneEnabled refuses for its own reasons -- no project, no scripting host, a
                // bridge with no Graph exports -- and says which in the log. Only claim the fallback
                // if an entity actually exists now.
                droneStartedByPlay_ = (droneEntity_ != scene::kInvalidEntity);
            }
            if (droneStartedByPlay_)
                AVER_INFO("[Sandbox] Play: no GameMode declared -- flying the default drone instead. "
                          "Declare an [AverGameMode] class to take over.");
            else
                AVER_WARN("[Sandbox] Play: no GameMode class is loaded, and the drone fallback could "
                          "not start either (see the reason logged above)");
            return;
        }
        const int32_t gi = aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_INSTANCE);  // 0 == none, allowed
        if (aver_fw_begin_play(gi, gm))
            AVER_INFO("[Sandbox] Play: begin_play GameMode='{}'{}", aver_fw_class_name(gm),
                      gi ? std::string(" GameInstance='") + aver_fw_class_name(gi) + "'" : std::string());
        else
            AVER_WARN("[Sandbox] Play: begin_play was rejected (already playing?)");
    }

    // True when Play is standing in a drone because the project declares no GameMode. Not a real
    // play session -- aver_fw_begin_play never ran -- so aver_fw_play_state() knows nothing about it
    // and every place that gates on "are we playing" has to ask this too.
    bool dronePlayActive() const { return droneStartedByPlay_; }

    // Ends whichever kind of play is running. Both kinds, deliberately: a drone the USER switched on
    // from Window > Drone is left alone, because stopping play should not take down something the
    // user started for their own reasons and never asked play to own.
    void stopPlay() {
        if (droneStartedByPlay_) {
            setDroneEnabled(false);
            droneStartedByPlay_ = false;
            AVER_INFO("[Sandbox] Stop: default drone stopped");
        }
        if (aver_fw_play_state() != AVER_FW_PLAY_EDITOR) {
            aver_fw_end_play();
            AVER_INFO("[Sandbox] Stop: play session ended");
        }
    }

    // Runs the --play-test session: begins play, drives synthetic input for 150 frames, then stops.
    void maybePlayTest() {
        if (!playTest_) return;
        if (!playTestBegun_) {
            if (aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_MODE) == 0) {
                if (++playTestWait_ > 10) { playTest_ = false;
                    AVER_WARN("[play-test] no GameMode class after 10 frames - pass --scripts <dir> with a GameMode"); }
                return;
            }
            playTestBegun_ = true;
            AVER_INFO("[play-test] starting - watch for GameMode/Controller/Pawn OnBeginPlay + Pawn OnTick");
            startPlay();
            return;
        }
        if (aver_fw_play_state() == AVER_FW_PLAY_PLAYING) {
            aver_fw_input_set_key(AVER_FW_KEY_W, 1);
            if (playTestFrames_ > 60) aver_fw_input_set_key(AVER_FW_KEY_MOUSE_LEFT, 1);
            if (playTestFrames_ == 100) aver_fw_input_set_key(AVER_FW_KEY_SPACE, 1);
            if (++playTestFrames_ == 150) {
                const int32_t pawn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
                if (pawn) {
                    const scene::Entity pe = static_cast<scene::Entity>(static_cast<uint32_t>(pawn));
                    const Mat4& wm = scene::World::instance().worldMatrix(pe);
                    AVER_INFO("[play-test] character walked to ({:.1f}, {:.1f}, {:.1f}) under synthetic W (spawned at origin)",
                              wm.m[3][0], wm.m[3][1], wm.m[3][2]);
                }
                AVER_INFO("[play-test] stopping - watch for OnEndPlay(reason=Stop) lines");
                aver_fw_end_play();
            }
        }
    }

    // Publishes this frame's keyboard and mouse into the framework, for the C# Input class.
    // Suppressed while ImGui wants the input, and the editor's drawer chord wins over gameplay.
    void pushInput(bool uiActive) {
        aver_fw_input_new_frame();
#if AVER_WITH_IMGUI
        if (!uiActive) return;
        if (releasedByUser_ && playSessionActive()) return;
        ImGuiIO& io = ImGui::GetIO();
        const bool kb = !io.WantCaptureKeyboard;
        const bool chordSpace = keybinds_.pressed(editor::CommandId::DrawerToggleContent, io);
        const bool chordEsc   = drawer_ != Drawer::None && keybinds_.pressed(editor::CommandId::DrawerDismiss, io);
        for (int i = 0; i < 26; ++i) aver_fw_input_set_key(AVER_FW_KEY_A + i, kb && ImGui::IsKeyDown((ImGuiKey)(ImGuiKey_A + i)));
        for (int i = 0; i < 10; ++i) aver_fw_input_set_key(AVER_FW_KEY_0 + i, kb && ImGui::IsKeyDown((ImGuiKey)(ImGuiKey_0 + i)));
        aver_fw_input_set_key(AVER_FW_KEY_SPACE,  kb && !chordSpace && ImGui::IsKeyDown(ImGuiKey_Space));
        aver_fw_input_set_key(AVER_FW_KEY_LSHIFT, kb && ImGui::IsKeyDown(ImGuiKey_LeftShift));
        aver_fw_input_set_key(AVER_FW_KEY_LCTRL,  kb && !chordSpace && ImGui::IsKeyDown(ImGuiKey_LeftCtrl));
        aver_fw_input_set_key(AVER_FW_KEY_LALT,   kb && ImGui::IsKeyDown(ImGuiKey_LeftAlt));
        aver_fw_input_set_key(AVER_FW_KEY_ENTER,  kb && ImGui::IsKeyDown(ImGuiKey_Enter));
        aver_fw_input_set_key(AVER_FW_KEY_ESCAPE, kb && !chordEsc && ImGui::IsKeyDown(ImGuiKey_Escape));
        aver_fw_input_set_key(AVER_FW_KEY_TAB,    kb && ImGui::IsKeyDown(ImGuiKey_Tab));
        aver_fw_input_set_key(AVER_FW_KEY_LEFT,   kb && ImGui::IsKeyDown(ImGuiKey_LeftArrow));
        aver_fw_input_set_key(AVER_FW_KEY_RIGHT,  kb && ImGui::IsKeyDown(ImGuiKey_RightArrow));
        aver_fw_input_set_key(AVER_FW_KEY_UP,     kb && ImGui::IsKeyDown(ImGuiKey_UpArrow));
        aver_fw_input_set_key(AVER_FW_KEY_DOWN,   kb && ImGui::IsKeyDown(ImGuiKey_DownArrow));
        const bool m = mouseCaptured_ || (!io.WantCaptureMouse && !io.WantCaptureKeyboard);
        aver_fw_input_set_key(AVER_FW_KEY_MOUSE_LEFT,   m && ImGui::IsMouseDown(0));
        aver_fw_input_set_key(AVER_FW_KEY_MOUSE_RIGHT,  m && ImGui::IsMouseDown(1));
        aver_fw_input_set_key(AVER_FW_KEY_MOUSE_MIDDLE, m && ImGui::IsMouseDown(2));
        if (mouseCaptured_) aver_fw_input_set_mouse(captureDx_, captureDy_, io.MouseWheel);
        else aver_fw_input_set_mouse(m ? io.MouseDelta.x : 0.0f, m ? io.MouseDelta.y : 0.0f, m ? io.MouseWheel : 0.0f);
#endif
    }

    // While playing, drives the view camera from the pawn's published view node, falling back to the
    // pawn's own matrix. Row 3 is the position, row 0 the forward (+X) axis.
    void drivePlayCamera() {
        scene::World& w = scene::World::instance();
        scene::Entity e = scene::kInvalidEntity;

        if (aver_fw_play_state() == AVER_FW_PLAY_PLAYING) {
            const int32_t pawn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
            if (pawn == 0) return;
            e = static_cast<scene::Entity>(static_cast<uint32_t>(pawn));
        } else if (dronePlayActive() && droneEntity_ != scene::kInvalidEntity) {
            // THE DRONE IS THE PAWN WHEN THERE IS NO GameMode, so the camera follows it like one.
            //
            // It never was before, and the reason is worth recording: this function gated on
            // aver_fw_controlled_pawn, the drone fallback never calls aver_fw_begin_play, so that
            // returned 0 and this returned immediately -- the drone flew and the camera sat wherever
            // it had been left. That is the whole of what "a viewable window" meant: something to
            // look at if you happened to be pointing at it, with no possession and no follow.
            //
            // Treated as the possessed pawn HERE rather than pushed through aver_fw_begin_play,
            // because a real possession needs a PlayerController and a pawn CLASS, and the fallback
            // exists precisely for projects that have declared neither. This gives the behaviour a
            // possession would give without inventing a fake one.
            e = droneEntity_;
        } else {
            return;
        }
        if (!w.valid(e)) return;

        int32_t mode = AVER_FW_VIEW_THIRD_PERSON; float eye = 160.0f, boom = 450.0f;
        aver_fw_view(&mode, &eye, &boom);

        // Prefer the view node; fall back to the pawn if it has not published one.
        const int32_t viewId = aver_fw_view_entity();
        const scene::Entity ve = static_cast<scene::Entity>(static_cast<uint32_t>(viewId));
        const bool haveView = viewId != 0 && w.valid(ve);
        const Mat4& vm = haveView ? w.worldMatrix(ve) : w.worldMatrix(e);
        const Mat4& pm = w.worldMatrix(e);

        const Vec3 headPos{vm.m[3][0], vm.m[3][1], vm.m[3][2]};
        const Vec3 headFwd = Vec3{vm.m[0][0], vm.m[0][1], vm.m[0][2]}.getSafeNormal();
        const Vec3 pawnPos{pm.m[3][0], pm.m[3][1], pm.m[3][2]};
        const Vec3 pawnFwd = Vec3{pm.m[0][0], pm.m[0][1], pm.m[0][2]}.getSafeNormal();
        const Vec3 up{0, 0, 1};

        Vec3 look;
        if (mode == AVER_FW_VIEW_FIRST_PERSON) {
            camPos_ = haveView ? headPos : pawnPos + up * eye;
            look    = headFwd;
        } else {
            const Vec3 pivot = haveView ? headPos : pawnPos + up * eye;
            const Vec3 armDir = haveView ? headFwd : pawnFwd;
            camPos_ = pivot - armDir * boom;
            look    = (pivot - camPos_).getSafeNormal();
        }
        // camForward() composes {cosP cosY, cosP sinY, sinP}; invert the look direction to yaw/pitch.
        yaw_   = std::atan2(look.y, look.x);
        pitch_ = std::asin(std::fmax(-1.0f, std::fmin(1.0f, look.z)));
    }
#endif

    // Aspect comes from the viewport rect (the dockspace's central node), not the whole window.
    f32 viewAspect() const { return vpH_ > 0.5f ? vpW_ / vpH_ : 1.777f; }
    bool inViewport(f32 mx, f32 my) const { return mx >= vpX_ && mx < vpX_+vpW_ && my >= vpY_ && my < vpY_+vpH_; }
    // The camera's forward axis, built from yaw and pitch.
    Vec3 camForward() const {
        return Vec3{ std::cos(pitch_)*std::cos(yaw_), std::cos(pitch_)*std::sin(yaw_), std::sin(pitch_) };
    }
    bool movableSelected() const { return sel_ >= 0 && sel_ < (int)objects_.size(); }

    // True while a play session owns the input, so editor interaction must stand down.
    bool gameHasInput() const { return playSessionActive() && !releasedByUser_; }

    // The gizmo's world-agnostic view of a selection's transform. Rotation is in Euler degrees.
    struct EditXform { Vec3 pos, rotDeg, scale; };

    // True when the selection is a thing in either world, rather than a sun/sky/post pseudo-entry.
    bool anySelected() const {
#if AVER_MODULE_SCENE
        if (sel_ == kSelScene) return scene::World::instance().valid(selEntity_);
#endif
        return movableSelected();
    }

    // Reads the selection's transform. False when nothing transformable is selected.
    bool selectedXform(EditXform& x) const {
#if AVER_MODULE_SCENE
        if (sel_ == kSelScene) {
            const scene::World& w = scene::World::instance();
            if (!w.valid(selEntity_)) return false;
            const auto* loc = w.component<scene::CLocal>(selEntity_, scene::kComponentLocal);
            if (!loc) return false;
            x.pos = loc->xf.position;
            x.rotDeg = eulerDegFromQuat(loc->xf.rotation);
            x.scale = loc->xf.scale;
            return true;
        }
#endif
        if (!movableSelected()) return false;
        const MeshObj& o = objects_[sel_];
        x.pos = o.pos; x.rotDeg = o.rotDeg; x.scale = o.scale;
        return true;
    }

    // Writes the selection's transform.
    void setSelectedXform(const EditXform& x) {
#if AVER_MODULE_SCENE
        if (sel_ == kSelScene) {
            scene::World& w = scene::World::instance();
            if (!w.valid(selEntity_)) return;
            Transform xf;
            xf.position = x.pos;
            xf.rotation = quatFromEulerDeg(x.rotDeg);
            xf.scale    = x.scale;
            w.setLocalTransform(selEntity_, xf);
            return;
        }
#endif
        if (!movableSelected()) return;
        MeshObj& o = objects_[sel_];
        o.pos = x.pos; o.rotDeg = x.rotDeg; o.scale = x.scale;
    }

    // Stable identity for an undoable object, so a command survives the entity being recreated.
    using EditId = u32;

    // One undoable edit: a transform change, or a create/destroy of either a scene entity or a
    // placeholder MeshObj (objects_), with everything needed to rebuild what was destroyed held by
    // value. CreateObj/DestroyObj are the MeshObj-world's own Create/Destroy -- the placeholder path
    // used to push no undo entry at all for an add or a delete (see deleteSelection()'s objects_
    // branch below); these two kinds close that gap without forcing MeshObj through the scene-entity
    // machinery it isn't part of.
    //
    // asset/meshId/material, which this struct used to carry directly, are gone: describeEntity()
    // now captures every component an entity has (not just CMeshRenderer) via EntitySnapshot --
    // see EditorEntitySnapshot.hpp for why that file exists and what it deliberately does not
    // capture (hierarchy; CName's internal blob offsets).
    struct EditCmd {
        enum class Kind { Transform, Create, Destroy, CreateObj, DestroyObj };
        Kind kind = Kind::Transform;
        EditId id = 0;            // a scene entity, through the indirection
        int objIndex = -1;        // or an objects_ index, for the placeholder scene
        EditXform before{}, after{};
        std::string label;        // outliner display name; editor-owned bookkeeping, not World's
#if AVER_MODULE_SCENE
        editor::EntitySnapshot snap;    // scene entity: asset name, persisted id, every other component
#endif
        bool hadBody = false;
        Vec3 bodyHalf{0,0,0};
        MeshObj objSnapshot{};    // CreateObj/DestroyObj payload; MeshObj is trivially copyable
    };

    // Returns the edit id bound to an entity, minting one on first use.
    //
    // AvId, NOT scene::Entity: this function (and entityForEdit/rebindEdit below) is called only
    // from #if AVER_MODULE_SCENE call sites, but the definitions themselves are unguarded, so with
    // SCENE off the parameter type needs to exist without the scene module. scene::Entity is a bare
    // type alias for AvId (aver/scene/Entity.hpp), so this is a zero-behaviour-change retype that
    // drops the dependency instead of widening the guard.
    EditId editIdFor(AvId e) {
        const u32 key = static_cast<u32>(e);
        if (const auto it = entityToEdit_.find(key); it != entityToEdit_.end()) return it->second;
        const EditId id = nextEditId_++;
        entityToEdit_[key] = id;
        editToEntity_[id]  = e;
        return id;
    }
    // Returns the entity an edit id names, or kInvalidId.
    AvId entityForEdit(EditId id) const {
        const auto it = editToEntity_.find(id);
        return it == editToEntity_.end() ? kInvalidId : it->second;
    }
    // Points an existing edit id at a newly created entity.
    void rebindEdit(EditId id, AvId e) {
        if (const auto old = editToEntity_.find(id); old != editToEntity_.end())
            entityToEdit_.erase(static_cast<u32>(old->second));
        editToEntity_[id] = e;
        entityToEdit_[static_cast<u32>(e)] = id;
    }

    // Pushes a command onto the undo stack and clears the redo stack.
    void pushEdit(EditCmd c) {
        undoStack_.push_back(std::move(c));
        redoStack_.clear();
        if (undoStack_.size() > kUndoDepth) undoStack_.erase(undoStack_.begin());
    }

    // Records the selection's transform before a gesture. False when nothing is selected.
    bool beginTransformEdit() {
        if (!selectedXform(editBefore_)) return false;
        editBeforeValid_ = true;
        return true;
    }
    // Closes the gesture and pushes a transform command, unless nothing actually moved.
    void endTransformEdit() {
        if (!editBeforeValid_) return;
        editBeforeValid_ = false;
        EditXform now;
        if (!selectedXform(now)) return;
        if (nearlySameXform(editBefore_, now)) return;
        EditCmd c;
        c.kind = EditCmd::Kind::Transform;
        c.before = editBefore_; c.after = now;
#if AVER_MODULE_SCENE
        if (sel_ == kSelScene) c.id = editIdFor(selEntity_); else
#endif
        c.objIndex = sel_;
        pushEdit(std::move(c));
    }
    // True when two transforms match to within 1e-4 on every component.
    static bool nearlySameXform(const EditXform& a, const EditXform& b) {
        auto same = [](const Vec3& p, const Vec3& q) {
            return std::fabs(p.x-q.x) < 1e-4f && std::fabs(p.y-q.y) < 1e-4f && std::fabs(p.z-q.z) < 1e-4f;
        };
        return same(a.pos,b.pos) && same(a.rotDeg,b.rotDeg) && same(a.scale,b.scale);
    }

    // Applies a transform command's stored value to whichever target it names, and selects it.
    void applyXformTo(const EditCmd& c, const EditXform& x) {
#if AVER_MODULE_SCENE
        if (c.id) {
            const scene::Entity e = entityForEdit(c.id);
            scene::World& w = scene::World::instance();
            if (e == scene::kInvalidEntity || !w.valid(e)) return;
            Transform xf; xf.position = x.pos; xf.rotation = quatFromEulerDeg(x.rotDeg); xf.scale = x.scale;
            w.setLocalTransform(e, xf);
            sel_ = kSelScene; selEntity_ = e;
            return;
        }
#endif
        if (c.objIndex >= 0 && c.objIndex < (int)objects_.size()) {
            MeshObj& o = objects_[c.objIndex];
            o.pos = x.pos; o.rotDeg = x.rotDeg; o.scale = x.scale;
            // Unguarded: this is the non-scene MeshObj fallback, reached with SCENE off too.
            sel_ = c.objIndex; selEntity_ = kInvalidId;
        }
    }

#if AVER_MODULE_SCENE
    // Describes a live entity fully enough to rebuild it after a destroy: its outliner label (the
    // editor's own bookkeeping, not World's), its transform, its physics-body half-extent if any,
    // and -- via captureEntity() -- its asset name, persisted object id, and every OTHER component
    // it carries, generically. Shared by Delete (undo), Copy and Duplicate: all three need exactly
    // "everything it would take to build this again", just for different reasons.
    EditCmd describeEntity(scene::Entity e) {
        scene::World& w = scene::World::instance();
        EditCmd c;
        c.id = editIdFor(e);
        if (const auto it = entityLabels_.find(static_cast<u32>(e)); it != entityLabels_.end()) c.label = it->second;
        if (const auto* loc = w.component<scene::CLocal>(e, scene::kComponentLocal)) {
            c.after.pos = loc->xf.position;
            c.after.rotDeg = eulerDegFromQuat(loc->xf.rotation);
            c.after.scale = loc->xf.scale;
        }
        c.snap = editor::captureEntity(w, e);
#if AVER_MODULE_PHYSICS
        if (const auto it = entityBodies_.find(static_cast<u32>(e)); it != entityBodies_.end()) {
            c.hadBody = true;
            c.bodyHalf = c.after.scale;
        }
#endif
        return c;
    }

    // Builds a scene entity from a snapshot at `xf`, wires it into the editor's own bookkeeping
    // (levelEntities_, entityLabels_, a physics body if it had one), selects it, and returns it (or
    // kInvalidEntity if the world refused). This is the shared tail recreateFrom()/pasteClipboard()/
    // duplicateSelection() all need -- generalized from CMeshRenderer-only to whatever the snapshot
    // captured, via EditorEntitySnapshot.hpp's instantiateEntity().
    //
    // restoreObjectId: true for recreateFrom (undo/redo -- the SAME logical entity must come back
    // with the SAME persisted identity), false for Paste/Duplicate (a NEW entity, which must NOT
    // clone the source's objectId -- see instantiateEntity()'s own comment for why that would be
    // wrong).
    //
    // spawnCube()/spawnFromAssetDrop() do NOT go through this: they build a fresh entity with no
    // prior snapshot to instantiate FROM, and refactoring their own working create tails onto this
    // path is out of scope here (real regression risk on two paths that already work, for no
    // required behaviour change) -- see the report this change shipped with.
    scene::Entity spawnEntityFrom(const editor::EntitySnapshot& snap, const EditXform& xf,
                                   const std::string& label, bool hadBody, const Vec3& bodyHalf,
                                   bool restoreObjectId = true) {
        scene::World& w = scene::World::instance();
        Transform t; t.position = xf.pos; t.rotation = quatFromEulerDeg(xf.rotDeg); t.scale = xf.scale;
        const scene::Entity e = editor::instantiateEntity(w, snap, t, scene::kInvalidEntity, restoreObjectId);
        if (e == scene::kInvalidEntity) {
            AVER_WARN("[Editor] the world refused to create '{}'", snap.asset);
            return e;
        }
        levelEntities_.push_back(e);
        if (!label.empty()) entityLabels_[static_cast<u32>(e)] = label;
#if AVER_MODULE_PHYSICS
        if (hadBody && aver_phys_ready()) {
            const int32_t body = aver_phys_add_static_box(t.position.x, t.position.y, t.position.z,
                                                          bodyHalf.x, bodyHalf.y, bodyHalf.z);
            if (body) aver_phys_set_entity(body, static_cast<int32_t>(e));
            levelBodies_.push_back(body);
            entityBodies_[static_cast<u32>(e)] = body;
        }
#endif
        sel_ = kSelScene; selEntity_ = e;
        return e;
    }

    // Rebuilds an entity a command destroyed and rebinds its EditId to the new handle -- the ONE
    // caller of spawnEntityFrom that must preserve the original EditId rather than mint a fresh one,
    // since a later redo/undo of the SAME command needs to keep finding the same logical entity.
    void recreateFrom(const EditCmd& c) {
        const scene::Entity e = spawnEntityFrom(c.snap, c.after, c.label, c.hadBody, c.bodyHalf);
        if (e == scene::kInvalidEntity) return;
        rebindEdit(c.id, e);
    }

    // Removes an entity and everything the editor hung off it, including its static body.
    void destroyEntity(scene::Entity e) {
        scene::World& w = scene::World::instance();
        if (!w.valid(e)) return;
        w.destroy(e);
        levelEntities_.erase(std::remove(levelEntities_.begin(), levelEntities_.end(), e), levelEntities_.end());
        entityLabels_.erase(static_cast<u32>(e));
#if AVER_MODULE_PHYSICS
        if (const auto it = entityBodies_.find(static_cast<u32>(e)); it != entityBodies_.end()) {
            aver_phys_remove_body(it->second);
            levelBodies_.erase(std::remove(levelBodies_.begin(), levelBodies_.end(), it->second), levelBodies_.end());
            entityBodies_.erase(it);
        }
#endif
    }
#endif  // AVER_MODULE_SCENE

    bool canUndo() const { return !undoStack_.empty(); }
    bool canRedo() const { return !redoStack_.empty(); }

    // Reverses the newest command and moves it to the redo stack.
    //
    // CreateObj/DestroyObj address objects_ directly by the index captured when the command was
    // pushed. That index is guaranteed valid when the command is reached: undo/redo only ever pop
    // the stack's back() (strict LIFO, see pushEdit()), so any entry pushed AFTER this one sits
    // above it and is necessarily undone first -- the vector is always back in exactly the state it
    // was in when this entry was captured. This depends on one invariant: nothing outside the undo
    // path may insert/erase objects_ without also clearing undoStack_/redoStack_ -- already true
    // today (level load/new-level clears both alongside objects_/levelEntities_), and now documented
    // as the rule any future objects_-mutating code must keep.
    // Keeps reflBeaconIndex_ pointing at the beacon when objects_ shifts underneath it.
    //
    // reflBeaconIndex_ is captured ONCE, as objects_.size()-1 at the moment --refl-test builds its
    // beacon, and then never maintained. Any erase BELOW it slid every later entry down one, so the
    // stored index went on addressing whatever had moved into that slot: the schedule at the draw
    // site then drove SOME OTHER object's `visible` flag on and off, silently, while the real beacon
    // sat frozen. The bounds check at the draw site keeps that in range, so it never crashed -- it
    // just measured the wrong thing, which is worse in a diagnostic whose entire job is measuring.
    //
    // Erasing the beacon itself invalidates the index rather than sliding it, because there is no
    // beacon left to point at and -1 is what the draw site already treats as "nothing to drive".
    void objectsErasedAt(int index) {
        if (reflBeaconIndex_ < 0) return;
        if (index == reflBeaconIndex_)     reflBeaconIndex_ = -1;
        else if (index <  reflBeaconIndex_) --reflBeaconIndex_;
    }
    void objectsInsertedAt(int index) {
        if (reflBeaconIndex_ >= 0 && index <= reflBeaconIndex_) ++reflBeaconIndex_;
    }

    void undo() {
        if (undoStack_.empty()) return;
        EditCmd c = undoStack_.back(); undoStack_.pop_back();
        switch (c.kind) {
            case EditCmd::Kind::Transform: applyXformTo(c, c.before); break;
#if AVER_MODULE_SCENE
            case EditCmd::Kind::Create:    destroyEntity(entityForEdit(c.id));
                                           sel_ = -1; selEntity_ = scene::kInvalidEntity; break;
            case EditCmd::Kind::Destroy:   recreateFrom(c); break;
#endif
            case EditCmd::Kind::CreateObj:   // undo a create: take it back out
                if (c.objIndex >= 0 && c.objIndex < (int)objects_.size()) {
                    objects_.erase(objects_.begin() + c.objIndex);
                    objectsErasedAt(c.objIndex);
                }
                sel_ = -1; selEntity_ = kInvalidId;
                break;
            case EditCmd::Kind::DestroyObj:  // undo a destroy: put it back at its old index
                if (c.objIndex >= 0 && c.objIndex <= (int)objects_.size()) {
                    objects_.insert(objects_.begin() + c.objIndex, c.objSnapshot);
                    objectsInsertedAt(c.objIndex);
                }
                sel_ = c.objIndex; selEntity_ = kInvalidId;
                break;
            default: break;   // Create/Destroy (scene) fall here when AVER_MODULE_SCENE is off
        }
        redoStack_.push_back(std::move(c));
    }

    // Re-applies the newest undone command and moves it back to the undo stack. See undo()'s own
    // comment for why CreateObj/DestroyObj's raw objIndex addressing is safe.
    void redo() {
        if (redoStack_.empty()) return;
        EditCmd c = redoStack_.back(); redoStack_.pop_back();
        switch (c.kind) {
            case EditCmd::Kind::Transform: applyXformTo(c, c.after); break;
#if AVER_MODULE_SCENE
            case EditCmd::Kind::Create:    recreateFrom(c); break;
            case EditCmd::Kind::Destroy:   destroyEntity(entityForEdit(c.id));
                                           sel_ = -1; selEntity_ = scene::kInvalidEntity; break;
#endif
            case EditCmd::Kind::CreateObj:   // redo a create: put it back
                if (c.objIndex >= 0 && c.objIndex <= (int)objects_.size())
                    objects_.insert(objects_.begin() + c.objIndex, c.objSnapshot);
                sel_ = c.objIndex; selEntity_ = kInvalidId;
                break;
            case EditCmd::Kind::DestroyObj:  // redo a destroy: take it back out
                if (c.objIndex >= 0 && c.objIndex < (int)objects_.size()) {
                    objects_.erase(objects_.begin() + c.objIndex);
                    objectsErasedAt(c.objIndex);
                }
                sel_ = -1; selEntity_ = kInvalidId;
                break;
            default: break;   // Create/Destroy (scene) fall here when AVER_MODULE_SCENE is off
        }
        undoStack_.push_back(std::move(c));
    }

    // Returns what the status bar calls the current selection.
    std::string selectionLabel() const {
#if AVER_MODULE_SCENE
        if (sel_ == kSelScene && scene::World::instance().valid(selEntity_)) {
            const auto it = entityLabels_.find(static_cast<u32>(selEntity_));
            if (it != entityLabels_.end()) return it->second;
            const std::string nm = scene::World::instance().name(selEntity_);
            return nm.empty() ? ("Entity " + std::to_string((u32)selEntity_)) : nm;
        }
#endif
        if (movableSelected()) return objects_[sel_].name;
        if (sel_ == -2) return "Directional Light (Sun)";
        if (sel_ == -3) return "Sky + Atmosphere";
        if (sel_ == -4) return "Post Process";
        return "nothing selected";
    }

    // Returns the selection's largest half-extent in cm, for framing and gizmo sizing.
    f32 selectedRadius() const {
        EditXform x;
        if (!selectedXform(x)) return kEditorCubeHalf;
        const f32 s = std::fmax(std::fabs(x.scale.x), std::fmax(std::fabs(x.scale.y), std::fabs(x.scale.z)));
#if AVER_MODULE_SCENE
        if (sel_ == kSelScene) return std::fmax(1.0f, s);
#endif
        if (movableSelected()) {
            const MeshObj& o = objects_[sel_];
            const Vec3 e{o.aabbMax.x - o.aabbMin.x, o.aabbMax.y - o.aabbMin.y, o.aabbMax.z - o.aabbMin.z};
            return std::fmax(1.0f, 0.5f * std::fmax(e.x, std::fmax(e.y, e.z)) * s);
        }
        return std::fmax(1.0f, s);
    }

    // ---- PlayerStart: where the player spawns in ----
    //
    // THE LEVEL FORMAT ALREADY HAD THE ANSWER AND NOBODY READ IT. OcWorldData carries
    // hasSpawn/spawnX/spawnY/spawnZ/spawnYaw; the parser fills it, the writer emits it, and a
    // repo-wide grep found exactly two production sites -- both of them that parser and that writer.
    // Nothing consulted it: not aver_fw_begin_play (which passes nullptr for the pawn's position),
    // not loadLevel (which carries it into levelHeader_ and never looks), not saveLevel (whose own
    // comment lists SPAWN among the fields that "ride through untouched"). A level could state where
    // the player starts and be ignored.
    //
    // ONE RECORD, SO ONE MARKER. SPAWN is a single scalar record, not a list like placements or
    // pcgVolumes, so PlayerStart must NOT become a second, independent thing that can disagree with
    // it. The marker is the editor's live, visible handle onto that one record -- spawned from it on
    // load, written back into it on save -- exactly the shape hasLevelSun_/hasLevelFog_ already have
    // for SUN and FOG. Adding a PlayerStart when one exists selects the existing one rather than
    // creating a rival.
    //
    // A TRANSIENT ENTITY, never pushed to levelEntities_, for the same reason the drone is not: it
    // must not also be saved as a PLACE record, which would make the marker a mesh placement AND a
    // spawn record at once -- two sources of truth again, one of them invisible.
#if AVER_MODULE_SCENE
    scene::Entity playerStart_ = scene::kInvalidEntity;
#endif
    f32 playerStartYaw_ = 0.0f;

    // The spawn transform a level should use: the marker if one is live, else the loaded SPAWN
    // record, else nothing. Returns false when the level declares no spawn at all.
    bool playerStartTransform(Vec3& outPos, f32& outYawDeg) const {
#if AVER_MODULE_SCENE
        const scene::World& w = scene::World::instance();
        if (playerStart_ != scene::kInvalidEntity && w.valid(playerStart_)) {
            outPos = w.localTransform(playerStart_).position;
            outYawDeg = playerStartYaw_;
            return true;
        }
#endif
        if (!levelHeader_.hasSpawn) return false;
        outPos = Vec3{static_cast<f32>(levelHeader_.spawnX), static_cast<f32>(levelHeader_.spawnY),
                      static_cast<f32>(levelHeader_.spawnZ)};
        outYawDeg = static_cast<f32>(levelHeader_.spawnYaw);
        return true;
    }

#if AVER_MODULE_SCENE
    // Creates the marker entity at a world position. Shared by "Add > Player Start" and by the
    // level loader when a file already carries a SPAWN record.
    scene::Entity makePlayerStart(const Vec3& at, f32 yawDeg) {
        scene::World& world = scene::World::instance();
        Transform xf;
        xf.position = at;
        xf.rotation = quatFromEulerDeg(Vec3{0.0f, 0.0f, yawDeg});
        xf.scale = Vec3{kEditorCubeHalf, kEditorCubeHalf, kEditorCubeHalf};

        // Drawn as the unit cube because that is the only primitive the editor is guaranteed to
        // have; the NAME is what makes it a PlayerStart. Deliberately not an asset path -- the
        // marker is never written as a PLACE record, so nothing will try to resolve it as a mesh
        // file, and the outliner shows a readable word instead of a path.
        static const std::string kPlayerStartName = "PlayerStart";
        const scene::Entity e = world.create(kPlayerStartName, scene::kInvalidEntity, xf);
        if (e == scene::kInvalidEntity) {
            AVER_WARN("[Editor] Player Start: the world refused a new entity");
            return scene::kInvalidEntity;
        }
        static const std::string kCubeAsset = "Meshes/cube.ocmesh";
        if (auto* mr = static_cast<scene::CMeshRenderer*>(
                world.addComponent(e, scene::kComponentMeshRenderer))) {
            mr->mesh = fnv1a64(std::string_view(kCubeAsset));
            mr->flags |= scene::kMeshRendererVisible;
            mr->aabbMin[0] = mr->aabbMin[1] = mr->aabbMin[2] = -1.0f;
            mr->aabbMax[0] = mr->aabbMax[1] = mr->aabbMax[2] =  1.0f;
        }
        entityLabels_[static_cast<u32>(e)] = "Player Start";
        playerStartYaw_ = yawDeg;
        return e;
    }
#endif

    // "Add > Player Start". One per level: a second call selects the one that exists rather than
    // creating a rival the save path would have to choose between.
    void addPlayerStart(Engine&) {
#if AVER_MODULE_SCENE
        scene::World& world = scene::World::instance();
        if (playerStart_ != scene::kInvalidEntity && world.valid(playerStart_)) {
            sel_ = kSelScene; selEntity_ = playerStart_;
            cbStatus_ = "This level already has a Player Start -- selected it";
            AVER_INFO("[Editor] Player Start already exists; selected it rather than adding a second");
            return;
        }
        Vec3 at = camPos_ + camForward() * kAddDistance;
        if (snapMove_) for (int k = 0; k < 3; ++k) (&at.x)[k] = snapf((&at.x)[k], moveSnap_);
        // Faces the way the camera is facing, which is what someone placing a spawn point means by
        // "the player starts here": atan2 of the forward vector, in the same +X-forward/+Y-right
        // frame the level format's yaw is authored in.
        const Vec3 f = camForward();
        const f32 yaw = degrees(std::atan2(f.y, f.x));
        playerStart_ = makePlayerStart(at, yaw);
        if (playerStart_ == scene::kInvalidEntity) return;
        sel_ = kSelScene; selEntity_ = playerStart_;
        AVER_INFO("[Editor] Player Start at ({:.0f}, {:.0f}, {:.0f}) yaw {:.0f}",
                  at.x, at.y, at.z, yaw);
#else
        (void)0;
#endif
    }

    // Spawns a cube in front of the camera, in whichever world owns the viewport, and selects it.
    void spawnCube(Engine&) {
        const Vec3 at = camPos_ + camForward() * kAddDistance;
#if AVER_MODULE_SCENE
        if (hideEditorScene_ || !levelPath_.empty()) {
            scene::World& world = scene::World::instance();
            Transform xf;
            xf.position = at;
            if (snapMove_) for (int k=0;k<3;++k) (&xf.position.x)[k] = snapf((&xf.position.x)[k], moveSnap_);
            xf.rotation = Quat{0,0,0,1};
            // FROZEN: scale is the half-extent in cm, matching .ocworld PLACEG against the unit cube.
            xf.scale = Vec3{kEditorCubeHalf, kEditorCubeHalf, kEditorCubeHalf};

            // FROZEN: the entity name is the asset path saveLevel writes and loadOcworld hashes back.
            static const std::string kCubeAsset = "Meshes/cube.ocmesh";
            const scene::Entity e = world.create(kCubeAsset, scene::kInvalidEntity, xf);
            if (e == scene::kInvalidEntity) { AVER_WARN("[Editor] Add: the world refused a new entity"); return; }
            if (auto* mr = static_cast<scene::CMeshRenderer*>(
                    world.addComponent(e, scene::kComponentMeshRenderer))) {
                mr->mesh = fnv1a64(std::string_view(kCubeAsset));
                mr->flags |= scene::kMeshRendererVisible;
                mr->aabbMin[0] = mr->aabbMin[1] = mr->aabbMin[2] = -1.0f;
                mr->aabbMax[0] = mr->aabbMax[1] = mr->aabbMax[2] =  1.0f;
            }
            levelEntities_.push_back(e);
            entityLabels_[static_cast<u32>(e)] = makeEntityLabel(std::string(), kCubeAsset);
            sel_ = kSelScene; selEntity_ = e;
            {
                EditCmd c = describeEntity(e);
                c.kind = EditCmd::Kind::Create;
                pushEdit(std::move(c));
            }
            AVER_INFO("[Editor] added cube entity #{} at ({:.0f}, {:.0f}, {:.0f})",
                      (u32)e, xf.position.x, xf.position.y, xf.position.z);
            return;
        }
#endif
        if (!cubeMesh_) return;
        MeshObj c; c.mesh = cubeMesh_; c.tris = cubeTris_;
        c.name = "Cube " + std::to_string(++spawnCount_);
        c.pos = at;
        if (snapMove_) for (int k=0;k<3;++k) (&c.pos.x)[k] = snapf((&c.pos.x)[k], moveSnap_);
        c.color[0]=0.72f; c.color[1]=0.72f; c.color[2]=0.74f; c.metallic=0.0f; c.roughness=0.6f;
        objects_.push_back(c);
        // Unguarded: the placeholder-world fallback, reached with SCENE off too.
        sel_ = (int)objects_.size() - 1; selEntity_ = kInvalidId;
        // Pushes a CreateObj undo entry -- this branch used to push NOTHING, which meant adding a
        // placeholder cube (the DEFAULT path whenever no level is loaded, per this function's own
        // `hideEditorScene_ || !levelPath_.empty()` guard above) was silently non-undoable. Mirrors
        // the scene branch's own describeEntity()+pushEdit() tail just above, minus the indirection
        // through EditId/World that only the scene entity needs.
        EditCmd edit;
        edit.kind = EditCmd::Kind::CreateObj;
        edit.objIndex = sel_;
        edit.objSnapshot = c;
        pushEdit(std::move(edit));
    }

#if AVER_WITH_IMGUI
// DRAG-AND-DROP FROM THE CONTENT BROWSER, hence UI-only: spawnFromAssetDrop's sole caller is the
// viewport's ImGui drop target, and dropWorldPoint exists only to serve it. Both lean on
// viewportRay/lowerExt, which live in the browser half of this file.
#if AVER_MODULE_SCENE
    // Finds a finite world point to drop an asset at, from a screen-space mouse position. Order:
    // nearest scene-entity hit, else the ground plane, else a fixed distance along the ray from the
    // camera. Z is up in this engine (see averAtmoCamAlt()), so the ground plane is Z = 0, not Y = 0.
    Vec3 dropWorldPoint(f32 screenX, f32 screenY) const {
        Vec3 ro, rd;
        viewportRay(screenX, screenY, ro, rd);

        f32 bestT = 1e30f; bool hit = false;
        if (!hideEditorScene_) {
            scene::World& w = scene::World::instance();
            const u32 n = w.count();
            for (u32 i = 0; i < n; ++i) {
                const scene::Entity ent = w.at(i);
                if (!w.valid(ent) || w.destroyPending(ent)) continue;
                const auto* mr = w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
                if (!mr || !(mr->flags & scene::kMeshRendererVisible) || mr->mesh == 0) continue;
                if (sceneMeshes_.find(mr->mesh) == sceneMeshes_.end()) continue;
                Vec3 lmin{mr->aabbMin[0], mr->aabbMin[1], mr->aabbMin[2]};
                Vec3 lmax{mr->aabbMax[0], mr->aabbMax[1], mr->aabbMax[2]};
                if (!(lmax.x > lmin.x && lmax.y > lmin.y && lmax.z > lmin.z)) { lmin = Vec3{-1,-1,-1}; lmax = Vec3{1,1,1}; }
                const Mat4 iw = w.worldMatrix(ent).inverse();
                const Vec3 lo = xformPoint(iw, ro), ld = xformVec(iw, rd);
                f32 t; if (rayAabb(lo, ld, lmin, lmax, t) && t > 0.0f && t < bestT) { bestT = t; hit = true; }
            }
        }
        if (hit) {
            const Vec3 p = ro + rd * bestT;
            if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) return p;
        }

        // Ground plane: Z = 0.
        if (std::fabs(rd.z) > 1e-6f) {
            const f32 t = -ro.z / rd.z;
            if (t > 0.0f) {
                const Vec3 p = ro + rd * t;
                if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) return p;
            }
        }

        // The ray is parallel to (or points away from) the ground: fall back to a fixed distance
        // in front of the camera, matching spawnCube()'s placement.
        return camPos_ + camForward() * kAddDistance;
    }

    // Places a content-browser asset dropped on the viewport at a screen-space position, following
    // spawnCube()'s create -> CMeshRenderer -> levelEntities_ -> undo recipe exactly. Only .ocmesh
    // assets are placeable in this slice; anything else is reported, not silently ignored.
    void spawnFromAssetDrop(Engine& e, const std::string& full, f32 screenX, f32 screenY) {
        const std::string ext = lowerExt(std::filesystem::path(full));
        const std::string fileName = std::filesystem::path(full).filename().string();
        const bool isMesh = ext == ".ocmesh";
        bool isParticle = false;
#if AVER_MODULE_PARTICLES
        isParticle = ext == ".ocparticle";
#endif
        if (!isMesh && !isParticle) {
#if AVER_MODULE_PARTICLES
            cbStatus_ = "'" + fileName + "' can't be placed in the level (only .ocmesh/.ocparticle assets can)";
            AVER_WARN("[Editor] drop: '{}' is not a placeable asset (need .ocmesh or .ocparticle)", full);
#else
            cbStatus_ = "'" + fileName + "' can't be placed in the level (only .ocmesh assets can)";
            AVER_WARN("[Editor] drop: '{}' is not a placeable asset (need .ocmesh)", full);
#endif
            return;
        }
        if (!(hideEditorScene_ || !levelPath_.empty())) {
            cbStatus_ = "Load a level (or hide the editor scene) before dropping assets";
            AVER_WARN("[Editor] drop: no scene world is active to place '{}' into", full);
            return;
        }

        const std::string content = project_.contentDir();
        std::error_code ec;
        std::string rel = content.empty() ? std::string()
                                          : std::filesystem::relative(full, content, ec).string();
        if (content.empty() || ec || rel.empty()) {
            cbStatus_ = "Could not resolve '" + fileName + "' to a project-relative path";
            AVER_WARN("[Editor] drop: relative() failed for '{}' against content root '{}'", full, content);
            return;
        }
        for (char& c : rel) if (c == '\\') c = '/';

#if AVER_MODULE_PARTICLES
        if (isParticle) {
            // A drop-to-place entry point for DECIDED 3's format, mirroring the .ocmesh path below
            // rather than growing its own copy of the drop-target/level-active checks above. Loaded
            // directly (not through loadProjectParticleEffects' whole-tree walk) so an effect just
            // authored -- possibly before any project (re)load has indexed it -- resolves immediately,
            // the same "the drop the user just made actually lands" reasoning the mesh path's own
            // comment gives for reloading synchronously rather than waiting a frame.
            particles::ParticleEffect fx;
            std::string err;
            if (!fmt::loadOcparticle(full, fx, nullptr, &err)) {
                cbStatus_ = "Could not load '" + fileName + "': " + err;
                AVER_WARN("[Editor] drop: {}", err);
                return;
            }
            const u64 effectId = fnv1a64(std::string_view(rel));
            particles::particleEffects().set(effectId, fx);

            const Vec3 at = dropWorldPoint(screenX, screenY);
            if (!(std::isfinite(at.x) && std::isfinite(at.y) && std::isfinite(at.z))) {
                cbStatus_ = "Could not find a valid drop position";
                AVER_WARN("[Editor] drop: computed a non-finite world position for '{}'", rel);
                return;
            }

            scene::World& world = scene::World::instance();
            Transform xf;
            xf.position = at;
            if (snapMove_) for (int k=0;k<3;++k) (&xf.position.x)[k] = snapf((&xf.position.x)[k], moveSnap_);

            const scene::Entity ent = world.create(rel, scene::kInvalidEntity, xf);
            if (ent == scene::kInvalidEntity) {
                cbStatus_ = "The world refused to place '" + rel + "'";
                AVER_WARN("[Editor] drop: the world refused a new entity for '{}'", rel);
                return;
            }
            if (auto* pe = static_cast<scene::CParticleEmitter*>(
                    world.addComponent(ent, scene::kComponentParticleEmitter))) {
                pe->effect = effectId;
            }
            levelEntities_.push_back(ent);
            entityLabels_[static_cast<u32>(ent)] = makeEntityLabel(std::string(), rel);
            sel_ = kSelScene; selEntity_ = ent;
            {
                EditCmd c = describeEntity(ent);
                c.kind = EditCmd::Kind::Create;
                pushEdit(std::move(c));
            }
            cbStatus_ = "Placed " + fileName;
            AVER_INFO("[Editor] placed particle emitter entity #{} from '{}' around effect 0x{:016X} "
                      "at ({:.0f}, {:.0f}, {:.0f})",
                      (u32)ent, rel, effectId, xf.position.x, xf.position.y, xf.position.z);
            return;
        }
#endif

        const u64 meshId = fnv1a64(std::string_view(rel));
        if (sceneMeshes_.find(meshId) == sceneMeshes_.end()) {
            // Not loaded yet -- most likely imported moments ago. Reload synchronously (the same
            // release+load pair buildUI() runs for wantMeshReload_) rather than waiting a frame, so
            // the drop the user just made actually lands.
            releaseProjectMeshes(e);
            loadProjectMeshes(e);
        }
        const auto meshIt = sceneMeshes_.find(meshId);
        if (meshIt == sceneMeshes_.end()) {
            cbStatus_ = "Could not resolve '" + rel + "' to a loaded mesh";
            AVER_WARN("[Editor] drop: '{}' (id {}) is not a loaded scene mesh", rel, meshId);
            return;
        }

        const Vec3 at = dropWorldPoint(screenX, screenY);
        if (!(std::isfinite(at.x) && std::isfinite(at.y) && std::isfinite(at.z))) {
            cbStatus_ = "Could not find a valid drop position";
            AVER_WARN("[Editor] drop: computed a non-finite world position for '{}'", rel);
            return;
        }

        scene::World& world = scene::World::instance();
        Transform xf;
        xf.position = at;
        if (snapMove_) for (int k=0;k<3;++k) (&xf.position.x)[k] = snapf((&xf.position.x)[k], moveSnap_);
        xf.rotation = Quat{0,0,0,1};
        xf.scale = Vec3{1,1,1};

        const scene::Entity ent = world.create(rel, scene::kInvalidEntity, xf);
        if (ent == scene::kInvalidEntity) {
            cbStatus_ = "The world refused to place '" + rel + "'";
            AVER_WARN("[Editor] drop: the world refused a new entity for '{}'", rel);
            return;
        }
        if (auto* mr = static_cast<scene::CMeshRenderer*>(
                world.addComponent(ent, scene::kComponentMeshRenderer))) {
            mr->mesh = meshId;
            mr->flags |= scene::kMeshRendererVisible;
            mr->aabbMin[0] = mr->aabbMin[1] = mr->aabbMin[2] = -1.0f;
            mr->aabbMax[0] = mr->aabbMax[1] = mr->aabbMax[2] =  1.0f;
        }
        levelEntities_.push_back(ent);
        entityLabels_[static_cast<u32>(ent)] = makeEntityLabel(std::string(), rel);
        sel_ = kSelScene; selEntity_ = ent;
        {
            EditCmd c = describeEntity(ent);
            c.kind = EditCmd::Kind::Create;
            pushEdit(std::move(c));
        }
        cbStatus_ = "Placed " + fileName;
        AVER_INFO("[Editor] placed entity #{} from '{}' at ({:.0f}, {:.0f}, {:.0f})",
                  (u32)ent, rel, xf.position.x, xf.position.y, xf.position.z);
    }
#endif  // AVER_WITH_IMGUI
#endif  // AVER_MODULE_SCENE

    f32 gizmoLen(const Vec3& origin) const { f32 L = dist(eye_, origin) * 0.17f; return L < 50.0f ? 50.0f : L; }

    // Projects a world point to viewport pixels (row-vector clip = p * viewProj). False when behind.
    bool project(const Vec3& wp, f32& sx, f32& sy) const {
        const Mat4& m = viewProj_;
        const f32 x = wp.x*m.m[0][0]+wp.y*m.m[1][0]+wp.z*m.m[2][0]+m.m[3][0];
        const f32 y = wp.x*m.m[0][1]+wp.y*m.m[1][1]+wp.z*m.m[2][1]+m.m[3][1];
        const f32 w = wp.x*m.m[0][3]+wp.y*m.m[1][3]+wp.z*m.m[2][3]+m.m[3][3];
        if (w <= 1e-4f) return false;
        sx = vpX_ + (x / w * 0.5f + 0.5f) * vpW_;
        sy = vpY_ + (1.0f - (y / w * 0.5f + 0.5f)) * vpH_;
        return true;
    }
    // Returns the distance from a point to a 2D line segment.
    static f32 distToSeg(f32 px, f32 py, f32 ax, f32 ay, f32 bx, f32 by) {
        const f32 vx=bx-ax, vy=by-ay, wx=px-ax, wy=py-ay;
        const f32 len2=vx*vx+vy*vy; f32 t = len2>1e-6f ? (wx*vx+wy*vy)/len2 : 0.0f;
        t = t<0?0:(t>1?1:t); const f32 cx=ax+vx*t, cy=ay+vy*t;
        return std::sqrt((px-cx)*(px-cx)+(py-cy)*(py-cy));
    }

    // The gizmo's three axes IN WORLD SPACE, for the current tool and the World/Local button.
    //
    // SCALE IS ALWAYS LOCAL, whatever the button says, and that is not an inconsistency. applyScale
    // writes o.scale.x/y/z, which ARE the object's own axes -- there is no other thing it could
    // write. Drawing that handle along a world axis on a rotated object meant the arrow you dragged
    // and the number that changed pointed in different directions. UE hides the world option on the
    // scale tool for the same reason; this keeps the button and just ignores it there.
    void gizmoBasis(const EditXform& o, Vec3 ax[3]) const {
        if (worldSpace_ && tool_ != Tool::Scale) {
            for (int a = 0; a < 3; ++a) ax[a] = kAxisDir[a];
            return;
        }
        const Quat q = quatFromEulerDeg(o.rotDeg);
        for (int a = 0; a < 3; ++a) ax[a] = q.rotate(kAxisDir[a]).getSafeNormal();
    }

    // Returns which gizmo handle is under the cursor: 0..2 axis, 3 = centre, -1 = none.
    int pickAxis(const Vec3& origin, const Vec3 ax[3], f32 L, f32 mx, f32 my) const {
        f32 ox, oy; if (!project(origin, ox, oy)) return -1;
        const f32 thr = 16.0f * dpi_;
        if (tool_ == Tool::Rotate) {
            int best=-1; f32 bestD=thr;
            for (int a=0;a<3;++a) {
                const Vec3 P=ax[(a+1)%3], Q=ax[(a+2)%3];
                f32 pxx=0, pyy=0; bool havePrev=false, first=true; f32 dmin=1e9f;
                const int N=48;
                for (int k=0;k<=N;++k) {
                    const f32 t=k*(kTwoPi/N);
                    Vec3 wp = origin + (P*std::cos(t) + Q*std::sin(t)) * L; f32 sx, sy;
                    if (project(wp, sx, sy)) { if (havePrev && !first) { f32 d=distToSeg(mx,my,pxx,pyy,sx,sy); if (d<dmin) dmin=d; } pxx=sx; pyy=sy; havePrev=true; first=false; }
                    else havePrev=false;
                }
                if (dmin<bestD) { bestD=dmin; best=a; }
            }
            return best;
        }
        if (std::sqrt((mx-ox)*(mx-ox)+(my-oy)*(my-oy)) < 13.0f*dpi_) return 3;
        int best=-1; f32 bestD=thr;
        for (int a=0;a<3;++a) {
            f32 tx, ty; if (!project(origin + ax[a]*L, tx, ty)) continue;
            const f32 d=distToSeg(mx,my,ox,oy,tx,ty);
            if (d<bestD) { bestD=d; best=a; }
        }
        return best;
    }

    // Moves the transform by a mouse delta in pixels, along the active axis or the screen plane.
    void applyMove(EditXform& o, f32 dx, f32 dy) {
        if (activeAxis_ == 3) {
            const Vec3 fwd = camForward();
            const Vec3 s = cross(Vec3{0,0,1}, fwd).getSafeNormal();
            const Vec3 u = cross(fwd, s);
            const f32 wpp = 2.0f * std::tan(radians(30.0f)) * dist(eye_, o.pos) / (vpH_ > 1 ? vpH_ : 900.0f);
            o.pos += s * (dx * wpp) + u * (-dy * wpp);
        } else {
            Vec3 ax[3]; gizmoBasis(o, ax);
            const Vec3 A = ax[activeAxis_];
            f32 s0x,s0y,s1x,s1y;
            if (project(o.pos, s0x, s0y) && project(o.pos + A, s1x, s1y)) {
                const f32 px=s1x-s0x, py=s1y-s0y, pl2=px*px+py*py;
                if (pl2 > 1e-4f) o.pos += A * ((dx*px + dy*py) / pl2);
            }
        }
        // Grid snap stays in WORLD space even for a local-axis drag. A grid the object is not
        // aligned to is still the grid the level is built on, and snapping to the object's own
        // rotated lattice would put nothing on round numbers.
        if (snapMove_) for (int k=0;k<3;++k) (&o.pos.x)[k] = snapf((&o.pos.x)[k], moveSnap_);
    }
    // Scales the transform by a mouse delta in pixels, along the active axis or uniformly.
    void applyScale(EditXform& o, f32 dx, f32 dy) {
        auto bump = [&](int a, f32 amt){ f32& c=(&o.scale.x)[a]; c += amt; if (c<0.02f) c=0.02f; };
        if (activeAxis_ == 3) { const f32 amt=(dx - dy)/80.0f; for (int a=0;a<3;++a) bump(a, amt); }
        else {
            Vec3 ax[3]; gizmoBasis(o, ax);
            const Vec3 A = ax[activeAxis_];
            f32 s0x,s0y,s1x,s1y;
            if (project(o.pos, s0x, s0y) && project(o.pos + A, s1x, s1y)) {
                const f32 px=s1x-s0x, py=s1y-s0y, pl=std::sqrt(px*px+py*py);
                if (pl > 1e-3f) bump(activeAxis_, ((dx*px + dy*py)/pl) / 60.0f);
            }
        }
        if (snapScale_) for (int k=0;k<3;++k) (&o.scale.x)[k] = std::fmax(0.02f, snapf((&o.scale.x)[k], scaleSnap_));
    }
    // Rotates the transform about the active gizmo axis by the angle the cursor swept around the
    // ring.
    //
    // BY QUATERNION COMPOSITION, not by adding to an Euler component. The old version did
    // `rotDeg[axis] += angle`, which is only the requested rotation when the other two components
    // are zero -- on an already-rotated object, dragging the ring you can SEE produced a rotation
    // about a different axis. Composition is exact in both spaces, and it is what lets the World
    // and Local buttons mean anything for this tool.
    //
    // The order is `dq * q`: this Quat's operator* is the Hamilton product and rotate() applies
    // q v q^-1, so composing "q first, then dq" is dq on the LEFT. Getting it backwards rotates
    // about the object's axes when you asked for the world's, and the two agree only at identity,
    // so it looks correct until the first time it matters. gizmoBasis already returns the axis in
    // WORLD space for both settings, so one order serves both.
    void applyRotate(EditXform& o, f32 px, f32 py, f32 mx, f32 my) {
        f32 ox, oy; if (!project(o.pos, ox, oy)) return;
        const f32 a0=std::atan2(py-oy, px-ox), a1=std::atan2(my-oy, mx-ox);
        f32 da=a1-a0; while (da> kPi) da-=kTwoPi; while (da< -kPi) da+=kTwoPi;

        Vec3 ax[3]; gizmoBasis(o, ax);
        const Vec3 A = ax[activeAxis_];
        // Which way the ring turns on screen depends on which side of it the camera is.
        const f32 sgn = dot(A, camForward()) >= 0.0f ? -1.0f : 1.0f;

        rotDragDeg_ += degrees(da) * sgn;
        // SNAPPING IS ON THE ACCUMULATED ANGLE, not the per-frame delta. Snapping each frame's
        // delta would round most of them to zero and the object would never turn.
        const f32 target = snapRot_ ? snapf(rotDragDeg_, rotSnap_) : rotDragDeg_;
        const f32 step = target - rotAppliedDeg_;
        if (std::fabs(step) < 1e-5f) return;
        rotAppliedDeg_ = target;

        const Quat dq = Quat::fromAxisAngle(A, radians(step));
        o.rotDeg = eulerDegFromQuat((dq * quatFromEulerDeg(o.rotDeg)).normalized());
    }

    // Runs the tool keys, picking, and the gizmo drag for one frame.
    void handleManip(Engine& e) {
#if AVER_WITH_IMGUI
        if (!e.device()->uiActive() || browserActive_) return;
        if (gameHasInput()) return;
        const ImGuiIO& io = ImGui::GetIO();

        if (levelFocused_ && !io.WantCaptureKeyboard) {
            // 1..4 select a tool WITHIN the active mode, so the same keys mean "the four things this
            // mode does" rather than being a single flat list that grows every time a mode is added.
            // Tab switches mode, which is the one binding that has to mean the same thing in both.
            // Both halves of the 1..4 dispatch are modelled as 4 commands, not 8 (ToolSelect..Scale
            // and SculptRaise..Flatten), whose scopes never overlap because mode_ can only be one
            // value at a time -- see EditorKeybinds.hpp's Scope comment.
            if (keybinds_.pressed(editor::CommandId::ModeToggleLandscape, io)) toggleEditorMode();
#if AVER_MODULE_LANDSCAPE
            if (mode_ == EditorMode::Landscape) {
                if (keybinds_.pressed(editor::CommandId::SculptRaise, io))   sculptTool_=SculptTool::Raise;
                if (keybinds_.pressed(editor::CommandId::SculptLower, io))   sculptTool_=SculptTool::Lower;
                if (keybinds_.pressed(editor::CommandId::SculptSmooth, io))  sculptTool_=SculptTool::Smooth;
                if (keybinds_.pressed(editor::CommandId::SculptFlatten, io)) sculptTool_=SculptTool::Flatten;
            } else
#endif
            {
                if (keybinds_.pressed(editor::CommandId::ToolSelect, io)) tool_=Tool::Select;
                if (keybinds_.pressed(editor::CommandId::ToolMove, io))   tool_=Tool::Move;
                if (keybinds_.pressed(editor::CommandId::ToolRotate, io)) tool_=Tool::Rotate;
                if (keybinds_.pressed(editor::CommandId::ToolScale, io))  tool_=Tool::Scale;
            }
#if AVER_MODULE_LANDSCAPE
            if (landscapeLoaded_) {
            }
#endif
        }
        const f32 mx=io.MousePos.x, my=io.MousePos.y;
        const bool overScene = levelHovered_ && inViewport(mx, my);

#if AVER_MODULE_LANDSCAPE
        const bool isSculptTool = editorModeIsLandscape();
#else
        // Unused with the module off -- the isSculptTool branch below compiles out along with it --
        // but declared anyway so isXformTool's "everything that is not a sculpt tool" phrasing does
        // not need its own second definition per configuration.
        [[maybe_unused]] const bool isSculptTool = false;
#endif
        // Only Move/Rotate/Scale ever show or drive the transform gizmo -- a sculpt tool has its own
        // brush-ring cursor (drawSculptCursor()) and its own click/drag handling below, not this one.
        // Object transforms are a Select-mode action, for the same reason. In Landscape mode a
        // drag is a brush stroke and must not also nudge whatever happens to be selected.
        const bool isXformTool = !editorModeIsLandscape() &&
                                 (tool_==Tool::Move || tool_==Tool::Rotate || tool_==Tool::Scale);

        hoverAxis_ = -1;
        EditXform gx;
        const bool haveGizmo = isXformTool && anySelected() && selectedXform(gx);
        Vec3 gaxis[3];
        if (haveGizmo) gizmoBasis(gx, gaxis);
        if (haveGizmo && !dragging_ && overScene)
            hoverAxis_ = pickAxis(gx.pos, gaxis, gizmoLen(gx.pos), mx, my);

#if AVER_MODULE_LANDSCAPE
        if (isSculptTool) {
            handleSculpt(e, io, overScene, mx, my);
        } else
#endif
        {
            if (ImGui::IsMouseClicked(0) && overScene) {
                int ax = -1;
                if (haveGizmo)
                    ax = pickAxis(gx.pos, gaxis, gizmoLen(gx.pos), mx, my);
                if (ax >= 0) {
                    dragging_=true; activeAxis_=ax; prevMouseX_=mx; prevMouseY_=my;
                    rotDragDeg_=0.0f; rotAppliedDeg_=0.0f;   // see applyRotate
                    beginTransformEdit();
                }
                else pick(e, io);
            }
            if (!io.MouseDown[0]) {
                if (dragging_) endTransformEdit();
                dragging_=false; activeAxis_=-1;
            }
        }

        if (levelFocused_ && !io.WantTextInput) {
            if (keybinds_.pressed(editor::CommandId::EditDelete, io))    deleteSelection();
            if (keybinds_.pressed(editor::CommandId::EditCopy, io))      copySelection();
            if (keybinds_.pressed(editor::CommandId::EditPaste, io))     pasteClipboard();
            if (keybinds_.pressed(editor::CommandId::EditDuplicate, io)) duplicateSelection();
            if (keybinds_.pressed(editor::CommandId::EditUndo, io)) undo();
            // Ctrl+Shift+Z: an intentionally NOT-rebindable alternate spelling of Redo (same command,
            // not a second one) -- kept as a small hardcoded fallback next to the registry-driven
            // checks, exactly as it was hardcoded before this file existed.
            if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)) redo();
            if (keybinds_.pressed(editor::CommandId::EditRedo, io)) redo();
        }

        if (dragging_ && anySelected()) {
            EditXform o;
            if (selectedXform(o)) {
                const f32 dx=mx-prevMouseX_, dy=my-prevMouseY_;
                if (tool_==Tool::Move)        applyMove(o, dx, dy);
                else if (tool_==Tool::Rotate) applyRotate(o, prevMouseX_, prevMouseY_, mx, my);
                else if (tool_==Tool::Scale)  applyScale(o, dx, dy);
                setSelectedXform(o);
            }
            prevMouseX_=mx; prevMouseY_=my;
        }
#else
        (void)e;
#endif
    }

#if AVER_MODULE_LANDSCAPE && AVER_WITH_IMGUI
    // Applies the active sculpt brush continuously while LMB is held over the terrain. Reuses
    // viewportRay() -- the SAME screen->ray conversion pick() and the asset drag-drop's drop point
    // use, per the task's own instruction not to reinvent it -- and landscape::raycastHeightfield()
    // for where that ray actually meets the section's surface.
    void handleSculpt(Engine& e, const ImGuiIO& io, bool overScene, f32 mx, f32 my) {
        sculptCursorValid_ = false;
        if (!landscapeLoaded_) { sculpting_ = false; return; }

        Vec3 ro, rd;
        viewportRay(mx, my, ro, rd);
        const f32 roA[3] = {ro.x, ro.y, ro.z}, rdA[3] = {rd.x, rd.y, rd.z};
        landscape::HeightfieldHit hit;
        const bool haveHit = overScene && landscape::raycastHeightfield(landscapeData_, roA, rdA, hit);
        if (haveHit) {
            sculptCursorValid_ = true;
            sculptCursor_ = Vec3{hit.posCm[0], hit.posCm[1], hit.posCm[2]};
        }

        if (ImGui::IsMouseClicked(0) && overScene && haveHit) {
            sculpting_ = true;
            sculptFlattenTargetCm_ = hit.posCm[2];   // captured once per stroke -- see BrushParams
        }
        if (!io.MouseDown[0]) sculpting_ = false;

        if (sculpting_ && haveHit) {
            landscape::BrushParams p;
            p.centerCm[0] = hit.posCm[0];
            p.centerCm[1] = hit.posCm[1];
            p.radiusCm = sculptRadiusCm_;
            p.strength = sculptStrengthCm_;
            p.flattenTargetCm = sculptFlattenTargetCm_;
            p.mode = sculptTool_==SculptTool::Raise  ? landscape::BrushMode::Raise
                   : sculptTool_==SculptTool::Lower  ? landscape::BrushMode::Lower
                   : sculptTool_==SculptTool::Smooth ? landscape::BrushMode::Smooth
                                                : landscape::BrushMode::Flatten;

            // dt-scaled so holding the button paints at a constant rate regardless of frame rate,
            // clamped the same way the other per-frame dt reads in this file are (see e.g. the fly
            // camera's own dt clamp) so a stall does not apply one giant, frame-skipping stroke.
            const f32 dt = std::fmin(e.time().dt, 0.05f);
            const f32 amount = std::fmin(1.0f, dt * 6.0f);

            const landscape::BrushRect touched = landscape::applyBrush(landscapeData_, p, amount);
            if (!touched.empty) {
                // REBUILDS THE WHOLE TREE, not just the touched nodes: LandscapeTree::build() has no
                // incremental form (a coarser node's errorCm/skirtCm/radius all depend on a per-level
                // MAXIMUM taken over every node at that level, so a local change can, in principle,
                // change any of them). It is proportional to the section's total sample count, not to
                // the brush footprint -- cheap at the sizes this editor has been run against, but a
                // real cost on a large section held under continuous painting. Only the GPU mesh cache
                // invalidation below is footprint-local.
                std::string why;
                if (landscapeTree_.build(landscapeData_, landscapeTree_.nodeQuads(), &why)) {
                    landscapeTree_.widenRimSkirts(2.0f * landscapeNoiseParams_.amplitudeCm);
                    landscapeTree_.resetHysteresis();
                    if (landscapeRenderer_)
                        landscapeRenderer_->forgetOverlapping(*e.device(), landscapeTree_,
                                                               touched.x0, touched.y0, touched.x1, touched.y1);
                } else {
                    AVER_ERROR("[Landscape] sculpt left the section unbuildable: {}", why);
                }
                landscapeDirty_ = true;
            }
        }
    }
#endif

    // Draws the current tool's gizmo over the selection, on top of geometry.
    void drawGizmo(Engine& e) {
        // Anything but Move/Rotate/Scale has no gizmo -- Select has none, and (see handleManip's
        // isXformTool) neither do the sculpt tools, which draw their OWN cursor via
        // drawSculptCursor() instead. Spelled as the allow-list the three real gizmo tools are,
        // rather than Select's own denylist of one, so a tool added later defaults to "no gizmo"
        // instead of silently inheriting the Scale gizmo the old `: gzScale_` fallback below would
        // have given it.
        // The gizmo belongs to Select mode. Without this it would keep drawing over the terrain
        // while a brush was active, because tool_ still holds whatever object tool was last used --
        // the mode changes what the viewport is for, it does not clear the other mode's state.
        if (editorModeIsLandscape()) return;
        if (tool_!=Tool::Move && tool_!=Tool::Rotate && tool_!=Tool::Scale) return;
        EditXform x;
        if (!selectedXform(x)) return;
        const Vec3 O = x.pos;
        const f32 L = gizmoLen(O);
        // The handles are drawn along the SAME axes pickAxis tests and applyMove drags. The line
        // meshes are built along the world axes, so local space rotates them here. Row-vector
        // order, matching Transform::matrix(): scale, then rotate, then translate.
        const bool localGizmo = !worldSpace_ || tool_ == Tool::Scale;
        const Mat4 w = localGizmo
            ? Mat4::scale(Vec3{L,L,L}) * Mat4::fromQuat(quatFromEulerDeg(x.rotDeg)) * Mat4::translation(O)
            : Mat4::scale(Vec3{L,L,L}) * Mat4::translation(O);
        const rhi::LineHandle* nrm = tool_==Tool::Move ? gzMove_ : tool_==Tool::Rotate ? gzRot_ : gzScale_;
        const rhi::LineHandle* hi  = tool_==Tool::Move ? gzMoveHi_ : tool_==Tool::Rotate ? gzRotHi_ : gzScaleHi_;
        e.device()->setLineDepth(false);
        for (int a=0;a<3;++a) {
            const bool active = (dragging_ && a==activeAxis_) || (!dragging_ && a==hoverAxis_);
            e.device()->drawLines(active ? hi[a] : nrm[a], &w.m[0][0]);
        }
        e.device()->setLineDepth(true);
    }

#if AVER_MODULE_LANDSCAPE
    // The sculpt brush's footprint, drawn over the terrain the same undepth-tested way drawGizmo()
    // draws over a selection -- it needs to read through the ground plane it is standing on.
    void drawSculptCursor(Engine& e) {
        if (!brushRing_ || !sculptCursorValid_) return;
        const bool sculptTool = editorModeIsLandscape();
        if (!sculptTool) return;
        const Mat4 w = Mat4::scale(Vec3{sculptRadiusCm_, sculptRadiusCm_, sculptRadiusCm_}) *
                       Mat4::translation(sculptCursor_);
        e.device()->setLineDepth(false);
        e.device()->drawLines(brushRing_, &w.m[0][0]);
        e.device()->setLineDepth(true);
    }
#endif

#if AVER_WITH_IMGUI
    // Removes the selected entity or placeholder object from the world, pushing an undo entry
    // either way. Pseudo-entries (sun/sky/post) are ignored -- sel_ never lands here for them.
    //
    // The objects_ branch below now pushes a DestroyObj entry; it used to push nothing at all,
    // which meant deleting a placeholder object -- the default path whenever no level is loaded,
    // per spawnCube()'s own `hideEditorScene_ || !levelPath_.empty()` condition -- was silently
    // non-undoable. Closing that gap is in scope for "Delete", not a new regression.
    void deleteSelection() {
#if AVER_MODULE_SCENE
        if (sel_ == kSelScene && selEntity_ != scene::kInvalidEntity) {
            scene::World& w = scene::World::instance();
            if (w.valid(selEntity_)) {
                AVER_INFO("[Editor] deleted entity #{} '{}'", (u32)selEntity_, w.name(selEntity_));
                EditCmd c = describeEntity(selEntity_);
                c.kind = EditCmd::Kind::Destroy;
                destroyEntity(selEntity_);
                pushEdit(std::move(c));
            }
            selEntity_ = scene::kInvalidEntity;
            sel_ = -1;
            return;
        }
#endif
        if (sel_ >= 0 && sel_ < (int)objects_.size()) {
            EditCmd c;
            c.kind = EditCmd::Kind::DestroyObj;
            c.objIndex = sel_;
            c.objSnapshot = objects_[sel_];
            objects_.erase(objects_.begin() + sel_);
            objectsErasedAt(sel_);
            pushEdit(std::move(c));
            sel_ = -1;
        }
    }

    // Copies the selection into the editor's own clipboard. A pure read: nothing changes in the
    // world, so nothing is pushed onto the undo stack. hasScene/hasObject are set exclusively of
    // each other, mirroring the existing loose pairing of sel_/selEntity_ rather than introducing a
    // variant type for two cases that are already mutually exclusive by construction.
    void copySelection() {
#if AVER_MODULE_SCENE
        if (sel_ == kSelScene && selEntity_ != scene::kInvalidEntity && scene::World::instance().valid(selEntity_)) {
            const EditCmd c = describeEntity(selEntity_);
            clipboard_.hasScene   = true;
            clipboard_.sceneSnap  = c.snap;
            clipboard_.sceneXform = c.after;
            clipboard_.hadBody    = c.hadBody;
            clipboard_.bodyHalf   = c.bodyHalf;
            clipboard_.hasObject  = false;
            return;
        }
#endif
        if (movableSelected()) {
            clipboard_.hasObject = true;
            clipboard_.object = objects_[sel_];
#if AVER_MODULE_SCENE
            clipboard_.hasScene = false;
#endif
        }
    }

    // Rebuilds the clipboard's contents in front of the camera -- the SAME "in front of camera"
    // convention spawnCube() and dropWorldPoint()'s own fallback already use -- and pushes a FRESH
    // Create/CreateObj entry, independent of whatever undo entry the ORIGINAL copied thing came
    // from. Does nothing (silently) when the clipboard is empty; the keybind dispatch that calls
    // this does not gate on clipboard state, so a no-op Paste-with-nothing-copied is expected, not
    // an error.
    void pasteClipboard() {
        Vec3 at = camPos_ + camForward() * kAddDistance;
        if (snapMove_) for (int k = 0; k < 3; ++k) (&at.x)[k] = snapf((&at.x)[k], moveSnap_);
#if AVER_MODULE_SCENE
        if (clipboard_.hasScene) {
            EditXform x = clipboard_.sceneXform;
            x.pos = at;
            const std::string label = makeEntityLabel(std::string(), clipboard_.sceneSnap.asset);
            const scene::Entity e = spawnEntityFrom(clipboard_.sceneSnap, x, label, clipboard_.hadBody, clipboard_.bodyHalf, /*restoreObjectId=*/false);
            if (e == scene::kInvalidEntity) return;
            EditCmd c = describeEntity(e);
            c.kind = EditCmd::Kind::Create;
            pushEdit(std::move(c));
            AVER_INFO("[Editor] pasted entity #{}", (u32)e);
            return;
        }
#endif
        if (clipboard_.hasObject) {
            MeshObj o = clipboard_.object;
            o.pos = at;
            o.name = makeEntityLabel(clipboard_.object.name, clipboard_.object.name);
            objects_.push_back(o);
            sel_ = (int)objects_.size() - 1; selEntity_ = kInvalidId;
            EditCmd c;
            c.kind = EditCmd::Kind::CreateObj;
            c.objIndex = sel_;
            c.objSnapshot = o;
            pushEdit(std::move(c));
        }
    }

    // Rebuilds the SELECTION (not the clipboard) offset by a small fixed delta from where it already
    // sits -- Duplicate makes a visible sibling next to the original, where Paste restores a place at
    // the copied transform; the two verbs get separately-reasoned placement rather than sharing one
    // policy. Never touches clipboard_, so Duplicate does not clobber whatever the user last copied.
    void duplicateSelection() {
        const f32 delta = snapMove_ ? moveSnap_ : kDuplicateOffset;
#if AVER_MODULE_SCENE
        if (sel_ == kSelScene && selEntity_ != scene::kInvalidEntity && scene::World::instance().valid(selEntity_)) {
            const EditCmd src = describeEntity(selEntity_);
            EditXform x = src.after;
            x.pos.x += delta; x.pos.y += delta;
            const std::string label = makeEntityLabel(std::string(), src.snap.asset);
            const scene::Entity e = spawnEntityFrom(src.snap, x, label, src.hadBody, src.bodyHalf, /*restoreObjectId=*/false);
            if (e == scene::kInvalidEntity) return;
            EditCmd c = describeEntity(e);
            c.kind = EditCmd::Kind::Create;
            pushEdit(std::move(c));
            AVER_INFO("[Editor] duplicated entity #{}", (u32)e);
            return;
        }
#endif
        if (movableSelected()) {
            MeshObj o = objects_[sel_];
            o.pos.x += delta; o.pos.y += delta;
            o.name = makeEntityLabel(objects_[sel_].name, objects_[sel_].name);
            objects_.push_back(o);
            sel_ = (int)objects_.size() - 1; selEntity_ = kInvalidId;
            EditCmd c;
            c.kind = EditCmd::Kind::CreateObj;
            c.objIndex = sel_;
            c.objSnapshot = o;
            pushEdit(std::move(c));
        }
    }

    // --undo-test [N]: a headless, in-process proof that Copy/Paste/Duplicate/Delete/Undo/Redo
    // actually work at runtime, not just that they compile or that EditorEntitySnapshot's capture/
    // restore round-trips in isolation (EditorEntitySnapshotTest already covers that). Fires N
    // frames into a --frames run (see the countdown in onUpdate), calls the REAL private methods
    // this file implements these commands with against a REAL scene::World and a REAL objects_
    // vector, asserts the world's actual state after every step, prints one PASS/FAIL line per
    // assertion tagged [undo-test], and exits the process with 0 (everything held) or 1 (something
    // didn't) -- never returns, so it never risks a window staying open past the check.
    //
    // Two phases, run back to back in one call: Phase A forces the scene-entity branch every editor
    // command has (hideEditorScene_=true) and exercises describeEntity/spawnEntityFrom/undo/redo
    // through it; Phase B forces the placeholder-object branch (hideEditorScene_=false, no level)
    // and exercises the SAME six commands against objects_/MeshObj -- the path that, before this
    // change, silently pushed no undo entry for Create or Destroy at all (see deleteSelection()'s
    // own comment). Phase B runs even with AVER_MODULE_SCENE off, since objects_ needs it.
    void runUndoTest(Engine& eng) {
        int failures = 0;
        auto check = [&](bool cond, const char* what) {
            if (cond) AVER_INFO("[undo-test] PASS: {}", what);
            else      { AVER_ERROR("[undo-test] FAIL: {}", what); ++failures; }
        };

#if AVER_MODULE_SCENE
        {
            scene::World& w = scene::World::instance();
            // flush() is what actually retires a destroy() and makes count()/valid() see it --
            // World.cpp: destroy() only sets a pending bit and queues the entity; the slot stays
            // "live" until the next flush() runs (that is also why an undo of the SAME destroy still
            // works: recreateFrom() doesn't need the slot freed, it just creates a new one and
            // rebinds the EditId). The normal per-frame loop calls flush() once a frame on its own
            // (see the call site above onUpdate's own doc comment); this test crams several destroys
            // into ONE frame, so it has to call flush() itself after each one to see the same
            // eventually-consistent state a human clicking Delete across several real frames would.
            const u32 base = w.count();
            hideEditorScene_ = true;   // forces spawnCube()'s scene-entity branch, see its own `if`

            spawnCube(eng);
            w.flush();
            const scene::Entity a1 = selEntity_;
            check(sel_ == kSelScene && w.valid(a1) && w.count() == base + 1, "spawnCube creates one scene entity");

            // A custom object id, deliberately NOT the fnv1a64(asset name) a fresh create() assigns
            // on its own -- World::create()/setName both compute that same default hash regardless
            // of restoreObjectId (confirmed against World.cpp/OcWorld.cpp), so two entities sharing
            // an asset name naturally share a default objectId. Only a CUSTOM value distinguishes
            // "this id was deliberately carried over" (undo/redo) from "this id is just whatever a
            // fresh create() computes" (paste/duplicate) -- which is exactly the distinction
            // restoreObjectId exists to make, so the test has to force it into being observable.
            const u64 customId = 0x00A5EA55u;
            w.setObjectId(a1, customId);
            check(w.objectId(a1) == customId, "setObjectId sets the custom id the rest of this phase checks for");

            deleteSelection();
            w.flush();
            check(!w.valid(a1) && w.count() == base, "deleteSelection removes the scene entity");

            undo();
            w.flush();
            const scene::Entity a2 = selEntity_;
            check(sel_ == kSelScene && w.valid(a2) && w.count() == base + 1, "undo restores the deleted entity");
            check(w.valid(a2) && w.objectId(a2) == customId, "undo restores the SAME (custom) persisted object id");

            redo();
            w.flush();
            check(w.count() == base, "redo re-deletes the restored entity");

            undo();
            w.flush();
            const scene::Entity a3 = selEntity_;
            check(sel_ == kSelScene && w.valid(a3) && w.count() == base + 1, "a second undo restores it again");
            check(w.valid(a3) && w.objectId(a3) == customId, "the custom object id survives a second undo too");

            copySelection();
            pasteClipboard();
            w.flush();
            const scene::Entity b1 = selEntity_;   // pasteClipboard() selects the pasted copy
            check(w.valid(b1) && b1 != a3 && w.count() == base + 2, "paste creates a second, distinct entity");
            check(w.valid(b1) && w.objectId(b1) != customId,
                  "paste's copy does NOT clone a's custom object id (see instantiateEntity's restoreObjectId)");

            duplicateSelection();   // duplicates b1, the current selection
            w.flush();
            const scene::Entity c1 = selEntity_;
            check(w.valid(c1) && c1 != b1 && w.count() == base + 3, "duplicate creates a third, distinct entity");
            check(w.valid(c1) && w.objectId(c1) != customId, "duplicate's copy also does not clone the custom object id");

            undo(); w.flush(); check(w.count() == base + 2, "unwind 1/3: undoes duplicate's Create");
            undo(); w.flush(); check(w.count() == base + 1, "unwind 2/3: undoes paste's Create");
            undo(); w.flush(); check(w.count() == base,     "unwind 3/3: undoes the original spawn's Create");
        }
#else
        AVER_INFO("[undo-test] AVER_MODULE_SCENE is off; skipping the scene-entity phase");
#endif
        {
            const usize objBase = objects_.size();
            hideEditorScene_ = false;   // forces spawnCube()'s placeholder-object branch instead
            sel_ = -1; selEntity_ = kInvalidId;

            spawnCube(eng);
            check(sel_ >= 0 && (usize)sel_ < objects_.size() && objects_.size() == objBase + 1,
                  "spawnCube (placeholder branch) adds one object");

            deleteSelection();
            check(objects_.size() == objBase, "deleteSelection removes the placeholder object");

            undo();
            check(objects_.size() == objBase + 1 && sel_ >= 0 && (usize)sel_ < objects_.size(),
                  "undo restores the deleted placeholder object -- THIS DID NOT EXIST before this change");

            redo();
            check(objects_.size() == objBase, "redo re-deletes the placeholder object");

            undo();
            check(objects_.size() == objBase + 1, "a second undo restores the placeholder object again");

            copySelection();
            pasteClipboard();
            check(objects_.size() == objBase + 2, "paste creates a second placeholder object");

            duplicateSelection();
            check(objects_.size() == objBase + 3, "duplicate creates a third placeholder object");

            undo(); check(objects_.size() == objBase + 2, "unwind 1/3: undoes duplicate's CreateObj");
            undo(); check(objects_.size() == objBase + 1, "unwind 2/3: undoes paste's CreateObj");
            undo(); check(objects_.size() == objBase,     "unwind 3/3: undoes the original spawn's CreateObj");
        }

        AVER_INFO("[undo-test] {} failure(s)", failures);
        std::exit(failures == 0 ? 0 : 1);
    }

    // --keybind-test write|read: a TWO-PROCESS proof that a keybind rebind survives a restart, and
    // that conflict detection actually refuses rather than silently stealing a chord -- the task's
    // own "write a bind, exit, relaunch, read it back" requirement, which a single process cannot
    // demonstrate (its in-memory KeybindRegistry would just keep working whether or not anything
    // reached disk). Run once with "write", then AGAIN as a genuinely separate process with "read":
    //   write: rebinds Edit.Copy to the free chord Ctrl+K (checking it really is Ctrl+C and really
    //          is free first), and separately attempts to rebind Edit.Paste onto Ctrl+Z -- Edit.
    //          Undo's own default chord -- which conflictWith() must refuse. Saves and exits.
    //   read:  a FRESH process, whose keybinds_ has only ever loaded from editor.ini (never called
    //          rebind() itself), checks Edit.Copy comes back as Ctrl+K -- proving the rebind
    //          persisted -- and Edit.Paste comes back as its default Ctrl+V, not Ctrl+Z -- proving
    //          the REFUSED rebind never reached disk in the first place.
    void runKeybindPersistTest(const std::string& mode) {
        int failures = 0;
        auto check = [&](bool cond, const char* what) {
            if (cond) AVER_INFO("[keybind-test] PASS: {}", what);
            else      { AVER_ERROR("[keybind-test] FAIL: {}", what); ++failures; }
        };
        using editor::CommandId;
        using editor::Chord;

        if (mode == "write") {
            check(editor::chordToString(keybinds_.chordFor(CommandId::EditCopy)) == "Ctrl+C",
                  "Edit.Copy starts at its compiled-in default (Ctrl+C)");

            const Chord ctrlZ{ImGuiKey_Z, true, false, false};   // Edit.Undo's own default chord
            const bool blocked = !keybinds_.rebind(CommandId::EditPaste, ctrlZ);
            check(blocked, "rebinding Edit.Paste to Ctrl+Z is REFUSED (Edit.Undo already holds it)");
            check(editor::chordToString(keybinds_.chordFor(CommandId::EditPaste)) == "Ctrl+V",
                  "the refused rebind left Edit.Paste's chord unchanged");

            const Chord ctrlK{ImGuiKey_K, true, false, false};   // not any command's default
            check(keybinds_.conflictWith(CommandId::EditCopy, ctrlK,
                                          editor::keybindDef(CommandId::EditCopy).scope) == CommandId::Count,
                  "Ctrl+K is free before the rebind");
            check(keybinds_.rebind(CommandId::EditCopy, ctrlK), "rebinding Edit.Copy to the free chord Ctrl+K succeeds");
            check(editor::chordToString(keybinds_.chordFor(CommandId::EditCopy)) == "Ctrl+K",
                  "Edit.Copy now reads back as Ctrl+K in THIS process' memory");

            keybinds_.saveToPrefs();
            editor::flushEditorPrefs();
            AVER_INFO("[keybind-test] wrote keybind.edit.copy=Ctrl+K to {}", editor::editorPrefsPath());
        } else if (mode == "read") {
            // loadEditorPreferences() already ran earlier this frame (see prefsLoaded_) and called
            // keybinds_.loadFromPrefs() -- everything below checks what THAT load produced, in a
            // process that has never called rebind() at all.
            check(editor::chordToString(keybinds_.chordFor(CommandId::EditCopy)) == "Ctrl+K",
                  "a FRESH process reads Edit.Copy back as Ctrl+K from editor.ini -- the rebind persisted");
            check(editor::chordToString(keybinds_.chordFor(CommandId::EditPaste)) == "Ctrl+V",
                  "Edit.Paste is still Ctrl+V in a fresh process -- the REFUSED rebind never reached disk");
        } else {
            AVER_ERROR("[keybind-test] unknown mode '{}' (want write|read)", mode);
            ++failures;
        }

        AVER_INFO("[keybind-test] {} failure(s)", failures);
        std::exit(failures == 0 ? 0 : 1);
    }

    // Unprojects a screen-space point within the viewport rect into a world-space ray. Shared by
    // pick() and the asset drag-drop drop point so there is exactly one screen->ray conversion.
    void viewportRay(f32 screenX, f32 screenY, Vec3& ro, Vec3& rd) const {
        const f32 nx = (screenX - vpX_) / vpW_ * 2.f - 1.f;   // NDC within the viewport rect
        const f32 ny = 1.f - (screenY - vpY_) / vpH_ * 2.f;
        const Mat4& iv = invVP_;
        const f32 rx = nx*iv.m[0][0]+ny*iv.m[1][0]+iv.m[2][0]+iv.m[3][0];
        const f32 ry = nx*iv.m[0][1]+ny*iv.m[1][1]+iv.m[2][1]+iv.m[3][1];
        const f32 rz = nx*iv.m[0][2]+ny*iv.m[1][2]+iv.m[2][2]+iv.m[3][2];
        const f32 rw = nx*iv.m[0][3]+ny*iv.m[1][3]+iv.m[2][3]+iv.m[3][3];
        const Vec3 farW{rx/rw, ry/rw, rz/rw};
        ro = eye_; rd = farW - eye_;
    }

    // Selects whatever the cursor's ray hits first, across both the placeholder and scene worlds.
    void pick(Engine& e, const ImGuiIO& io) {
        (void)e;
        Vec3 ro, rd;
        viewportRay(io.MousePos.x, io.MousePos.y, ro, rd);
        int best=-1; f32 bestT=1e30f;
        if (!hideEditorScene_)
            for (int i=0;i<(int)objects_.size();++i){
                MeshObj& o=objects_[i]; if(!o.visible) continue;
                Transform tr; tr.position=o.pos; tr.rotation=quatFromEulerDeg(o.rotDeg); tr.scale=o.scale;
                const Mat4 iw = tr.toMatrix().inverse();
                const Vec3 lo=xformPoint(iw,ro), ld=xformVec(iw,rd);
                f32 t; if (rayAabb(lo,ld,o.aabbMin,o.aabbMax,t) && t<bestT){ bestT=t; best=i; }
            }

        // AvId, not scene::Entity: pick() spans both the placeholder and scene worlds, so bestEnt is
        // read and compared unguarded below even though only the loop that can set it away from
        // "nothing" is scene-only.
        AvId bestEnt = kInvalidId;
#if AVER_MODULE_SCENE
        {
            scene::World& w = scene::World::instance();
            const u32 n = w.count();
            for (u32 i = 0; i < n; ++i) {
                const scene::Entity ent = w.at(i);
                if (!w.valid(ent) || w.destroyPending(ent)) continue;
                // Streamed entities are not selectable: selection is the only door into the
                // gizmo/EditCmd path, and an entity the streamer can evict out from under an
                // in-flight edit or an undo record must never go through that door. See
                // setChunkStreamingEnabled and buildPanels (World Outliner) for the same rule.
                if (anyChunkWorldOwns(ent)) continue;
                const auto* mr = w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
                if (!mr || !(mr->flags & scene::kMeshRendererVisible) || mr->mesh == 0) continue;
                if (sceneMeshes_.find(mr->mesh) == sceneMeshes_.end()) continue;
                Vec3 lmin{mr->aabbMin[0], mr->aabbMin[1], mr->aabbMin[2]};
                Vec3 lmax{mr->aabbMax[0], mr->aabbMax[1], mr->aabbMax[2]};
                if (!(lmax.x > lmin.x && lmax.y > lmin.y && lmax.z > lmin.z)) { lmin = Vec3{-1,-1,-1}; lmax = Vec3{1,1,1}; }
                const Mat4 iw = w.worldMatrix(ent).inverse();
                const Vec3 lo = xformPoint(iw, ro), ld = xformVec(iw, rd);
                f32 t; if (rayAabb(lo, ld, lmin, lmax, t) && t < bestT) { bestT = t; bestEnt = ent; best = -1; }
            }
        }
#endif
        if (bestEnt != kInvalidId) { sel_ = kSelScene; selEntity_ = bestEnt; }
        else                       { sel_ = best;     selEntity_ = kInvalidId; }
    }

    // Draws a button with a drop-down triangle. Returns true when clicked.
    bool dropButton(const char* label) {
        const f32 extra = 16.0f*dpi_;
        const ImVec2 ts = ImGui::CalcTextSize(label);
        const bool clicked = ImGui::Button(label, ImVec2(ts.x + ImGui::GetStyle().FramePadding.x*2 + extra, 0));
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        const f32 cx = mx.x - extra*0.5f - 2.0f*dpi_, cy = (mn.y+mx.y)*0.5f, s = 3.0f*dpi_;
        ImGui::GetWindowDrawList()->AddTriangleFilled(
            ImVec2(cx-s,cy-s*0.55f), ImVec2(cx+s,cy-s*0.55f), ImVec2(cx,cy+s*0.8f), ImGui::GetColorU32(ImGuiCol_Text));
        return clicked;
    }

#endif

    // Records the outcome of an upgrade decision and restarts its time on screen.
    //
    // THROUGH A SETTER so the timer cannot be forgotten at one of the three call sites. The string
    // used to be assigned and never displayed anywhere: the user clicked Upgrade, the modal closed,
    // and whether it had worked was reported only to the log.
    void setUpgradeStatus(std::string msg) {
        upgradeStatus_ = std::move(msg);
        upgradeStatusAge_ = 0.0f;
    }

    // Exit, unless something is unsaved -- in which case ASK first.
    //
    // Every exit used to call requestExit() straight through. AssetEditorHost::anyDirty() existed
    // for exactly this check and had no callers, so the prompt it was written to drive never
    // appeared and closing the editor with an unsaved material silently discarded it.
    void requestExitChecked(Engine& e) {
#if AVER_WITH_IMGUI
        if (assetEditors_.anyDirty()) { exitPrompt_ = true; return; }
#endif
        e.requestExit();
    }

    // The unsaved-changes modal. Names the files, because "you have unsaved changes" is not
    // something a user can act on.
    void drawExitPrompt(Engine& e) {
#if AVER_WITH_IMGUI
        if (!exitPrompt_) return;
        constexpr const char* kTitle = "Unsaved changes";
        if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
        const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(480.0f * dpi_, 0.0f), ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

        const std::vector<std::string> dirty = assetEditors_.dirtyTitles();
        ImGui::TextWrapped("%zu open editor%s ha%s unsaved changes:",
                           dirty.size(), dirty.size() == 1 ? "" : "s", dirty.size() == 1 ? "s" : "ve");
        ImGui::Spacing();
        for (const std::string& t : dirty) ImGui::BulletText("%s", t.c_str());
        if (!exitPromptError_.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "%s", exitPromptError_.c_str());
        }
        ImGui::Spacing();
        ImGui::Separator();

        if (ImGui::Button("Save all and exit", ImVec2(150.0f * dpi_, 0.0f))) {
            std::string why;
            const usize failed = assetEditors_.saveAllDirty(&why);
            if (failed == 0) {
                ImGui::CloseCurrentPopup();
                exitPrompt_ = false;
                e.requestExit();
            } else {
                // STAY OPEN on a failed save. Exiting anyway would discard exactly the work the
                // user just asked to keep.
                exitPromptError_ = why;
                AVER_ERROR("[Editor] {} editor(s) could not be saved; the exit was cancelled: {}",
                           failed, why);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard and exit", ImVec2(150.0f * dpi_, 0.0f))) {
            AVER_WARN("[Editor] exiting with {} unsaved editor(s); the changes are gone", dirty.size());
            ImGui::CloseCurrentPopup();
            exitPrompt_ = false;
            e.requestExit();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.0f * dpi_, 0.0f))) {
            ImGui::CloseCurrentPopup();
            exitPrompt_ = false;
            exitPromptError_.clear();
        }
        ImGui::EndPopup();
#else
        (void)e;
#endif
    }

    // Draws the modal offering to add the project files this project is missing, listing each fix.
    void drawUpgradePrompt() {
#if AVER_WITH_IMGUI
        if (pendingUpgrade_.empty() || upgradeAsked_) return;
        constexpr const char* kTitle = "Upgrade project?";
        if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);

        const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(560.0f * dpi_, 0.0f), ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

        ImGui::TextWrapped("'%s' was created by an earlier version of the editor and is missing some "
                           "of the files a project now needs.", project_.name.c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("Nothing you wrote is replaced. The .csproj is edited by appending, not "
                            "regenerated, so anything you added by hand stays.");
        ImGui::Spacing();
        ImGui::Separator();

        if (ImGui::BeginChild("##upgradeList", ImVec2(0.0f, 200.0f * dpi_), ImGuiChildFlags_Borders)) {
            for (const editor::ProjectFix& f : pendingUpgrade_.fixes) {
                ImGui::BulletText("%s", f.summary.c_str());
                if (!f.detail.empty()) {
                    ImGui::Indent();
                    ImGui::TextDisabled("%s", f.detail.c_str());
                    ImGui::Unindent();
                }
            }
        }
        ImGui::EndChild();
        ImGui::Separator();

        if (ImGui::Button("Upgrade", ImVec2(120.0f * dpi_, 0.0f))) {
            std::string err;
            if (editor::applyProjectUpgrade(project_, pendingUpgrade_, &err)) {
                setUpgradeStatus("Project upgraded - Compile C# to rebuild.");
                AVER_INFO("[Editor] '{}' upgraded", project_.name);
            } else {
                setUpgradeStatus("Upgrade failed: " + err);
                AVER_ERROR("[Editor] upgrade of '{}' failed: {}", project_.name, err);
            }
            pendingUpgrade_ = {};
            upgradeAsked_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Not now", ImVec2(120.0f * dpi_, 0.0f))) {
            upgradeAsked_ = true;
            setUpgradeStatus("Project left as it is.");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Not now leaves every file untouched.");
        ImGui::EndPopup();
#endif
    }

    // Builds the whole editor UI for one frame: menu bar, toolbars, panels, drawers and dialogs.
    void buildUI(Engine& e) {
#if AVER_WITH_IMGUI
        uiReg_.beginFrame();
#endif
        prefsDevice_ = e.device();
#if AVER_WITH_IMGUI
        // Editor preferences are read/written by the preferences PANEL; a UI-less editor never opens
        // one, so the defaults compiled into the members stand.
        if (!prefsLoaded_) { prefsLoaded_ = true; loadEditorPreferences(); }
#endif
#if AVER_MODULE_SCENE
        if (wantMeshReload_) {
            wantMeshReload_ = false;
            releaseProjectMeshes(e);
            loadProjectMeshes(e);
        }
#endif
        if (vsyncOffRequested_) {
            vsyncOffRequested_ = false;
            if (prefsDevice_->vsyncCanDisable()) { prefsDevice_->setVSync(false); AVER_INFO("[Sandbox] vsync OFF (--no-vsync)"); }
            else AVER_WARN("[Sandbox] --no-vsync ignored: this display path cannot tear");
        }
#if AVER_WITH_IMGUI
        if (!e.device()->uiActive()) return;
        ++frameNo_;   // the Content Browser's directory-cache freshness clock

        if (browserActive_) {
            switch (browser_.draw(dpi_, fontMedium_, logoUiId_, logoAspect_)) {
                case editor::BrowserAction::Open: applyProject(e); browserActive_ = false; break;
                case editor::BrowserAction::Skip: browserActive_ = false; break;
                case editor::BrowserAction::Quit: requestExitChecked(e); break;
                case editor::BrowserAction::Stay: break;
            }
            return;
        }

        // Drawer shortcuts: Ctrl+Space toggles the Content Browser, Escape closes an open drawer.
        {
            const ImGuiIO& io = ImGui::GetIO();
            if (!io.WantTextInput && !io.WantCaptureKeyboard) {
                if (keybinds_.pressed(editor::CommandId::DrawerToggleContent, io)) toggleDrawer(Drawer::Content);
                if (drawer_ != Drawer::None && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
                    keybinds_.pressed(editor::CommandId::DrawerDismiss, io))
                    drawer_ = Drawer::None;
            }
        }

        // ---------------- menu bar ----------------
        if (ImGui::BeginMainMenuBar()) {
            if (fontMedium_) ImGui::PushFont(fontMedium_, 0.0f);
            ImGui::TextColored(ImVec4(0.95f,0.42f,0.13f,1),"AE");
                        const bool open_file = ImGui::BeginMenu("File");
            uiReg_.track("menu.file");
            if (open_file){
#if AVER_MODULE_SCENE
                const bool haveProject = project_.valid();
                ImGui::BeginDisabled(!haveProject);
                if (ImGui::MenuItem("New Level")) { unloadLevel(e); levelName_ = "untitled"; }
                uiReg_.track("file.newLevel");
                if (ImGui::MenuItem("Open Level")) {
                    loadStartMap(e);
                    // GRAPH-AS-CLASS / any other class placement: loadStartMap -> loadLevel already
                    // collects classPlacements_ (see that member's own comment), but does not spawn
                    // them -- applyProject's own "Starting scripts" stage is normally what calls
                    // spawnClassPlacements() after a fresh project open. This menu item reloads the
                    // level WITHOUT going through applyProject at all, so without this call a level
                    // with class placements would load with none of them spawned and no warning
                    // either -- silent, and exactly the failure mode this whole slice exists to avoid.
                    // Scripting (and any graph class it declared) is already up by the time a human can
                    // click this menu, so no CLI-style catch-up ordering concern applies here.
#if AVER_MODULE_FRAMEWORK
#if AVER_MODULE_SCRIPTING
                    if (scripts_.ready())
#endif
                        spawnClassPlacements();
#endif
                }
                uiReg_.track("file.openLevel");
                if (ImGui::MenuItem("Save Level", "Ctrl+S") && !levelPath_.empty()) saveLevel(levelPath_);
                uiReg_.track("file.saveLevel");
                ImGui::EndDisabled();
                if (!haveProject && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Open or create a project first - a level belongs to one.");
#else
                ImGui::MenuItem("New Level"); ImGui::MenuItem("Open Level..."); ImGui::MenuItem("Save Level");
#endif
#if AVER_MODULE_LANDSCAPE
                // Independent of the Save Level item above: a sculpt changes landscapeData_ in
                // memory only (see handleSculpt) -- the SAME split saveLevel/Save Level already has
                // between "the editor's state" and "what is actually on disk". Disabled with the
                // same specific-reason convention Package Project uses just below.
                {
                    const bool canSave = landscapeLoaded_ && !landscapePath_.empty();
                    ImGui::BeginDisabled(!canSave);
                    if (ImGui::MenuItem(landscapeDirty_ ? "Save Landscape *" : "Save Landscape")) saveLandscape();
                    ImGui::EndDisabled();
                    uiReg_.track("file.saveLandscape");
                    if (!canSave && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip(!landscapeLoaded_
                            ? "No landscape section loaded (--landscape, or <levelname>.ocland beside the level)."
                            : "This section has no file path to save back to.");
                }
#endif
                ImGui::Separator(); if(ImGui::MenuItem("Exit")) requestExitChecked(e); ImGui::EndMenu(); }
                        const bool open_edit = ImGui::BeginMenu("Edit");
            uiReg_.track("menu.edit");
            if (open_edit){
                if (ImGui::MenuItem("Undo", editor::chordToString(keybinds_.chordFor(editor::CommandId::EditUndo)).c_str(), false, canUndo())) undo();
                uiReg_.track("edit.undo");
                if (ImGui::MenuItem("Redo", editor::chordToString(keybinds_.chordFor(editor::CommandId::EditRedo)).c_str(), false, canRedo())) redo();
                uiReg_.track("edit.redo");
                ImGui::Separator();
                // Shortcut hints are read from the SAME registry the keypress dispatch in
                // handleManip() reads from, so a rebind can never leave the menu and the keyboard
                // disagreeing about what a chord does.
                if (ImGui::MenuItem("Copy", editor::chordToString(keybinds_.chordFor(editor::CommandId::EditCopy)).c_str(), false, anySelected())) copySelection();
                uiReg_.track("edit.copy");
                if (ImGui::MenuItem("Paste", editor::chordToString(keybinds_.chordFor(editor::CommandId::EditPaste)).c_str(), false, clipboard_.hasScene || clipboard_.hasObject)) pasteClipboard();
                uiReg_.track("edit.paste");
                if (ImGui::MenuItem("Duplicate", editor::chordToString(keybinds_.chordFor(editor::CommandId::EditDuplicate)).c_str(), false, anySelected())) duplicateSelection();
                uiReg_.track("edit.duplicate");
                if (ImGui::MenuItem("Delete", editor::chordToString(keybinds_.chordFor(editor::CommandId::EditDelete)).c_str(), false, anySelected())) deleteSelection();
                uiReg_.track("edit.delete");
                ImGui::Separator();
                if (ImGui::MenuItem("Editor Preferences...")) showEditorPrefs_ = true;
                uiReg_.track("edit.editorPreferences");
                if (ImGui::MenuItem("Project Settings...")) showProjectSettings_ = true;
                uiReg_.track("edit.projectSettings");
                ImGui::EndMenu();
            }
                        const bool open_window = ImGui::BeginMenu("Window");
            uiReg_.track("menu.window");
            if (open_window){
                ImGui::MenuItem("World Outliner"); ImGui::MenuItem("Details");
                if (ImGui::MenuItem("Content Browser", "Ctrl+Space", drawer_ == Drawer::Content)) toggleDrawer(Drawer::Content);
                uiReg_.track("window.contentBrowser");
                if (ImGui::MenuItem("Output Log", nullptr, drawer_ == Drawer::Log)) toggleDrawer(Drawer::Log);
                uiReg_.track("window.outputLogDup");
                if (ImGui::MenuItem("World Settings", nullptr, showWorldSettings_))
                    showWorldSettings_ = !showWorldSettings_;
                uiReg_.track("window.worldSettings");
                uiReg_.track("window.outputLog");
                ImGui::Separator();
#if AVER_MODULE_SCENE
                {
                    const bool streamOn = chunkWorld_ != nullptr;
                    ImGui::BeginDisabled(!project_.valid());
                    if (ImGui::MenuItem("Chunk Streaming", nullptr, streamOn)) setChunkStreamingEnabled(!streamOn);
                    ImGui::EndDisabled();
                    uiReg_.track("window.chunkStreaming");
                    if (!project_.valid() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("Open or create a project first - a streamed world belongs to one.");
                    else if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(streamOn
                            ? "Streams chunks generated + saved under <project>\\Chunks around the editor camera.\nTurning this off releases every streamed entity."
                            : "Opt-in: streams chunks generated + saved under <project>\\Chunks around the editor camera.\nDoes nothing to your level until switched on.");
                }
                {
                    const bool droneOn = droneEntity_ != scene::kInvalidEntity;
                    ImGui::BeginDisabled(!project_.valid());
                    if (ImGui::MenuItem("Drone", nullptr, droneOn)) setDroneEnabled(!droneOn);
                    ImGui::EndDisabled();
                    uiReg_.track("window.drone");
                    if (!project_.valid() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("Open or create a project first - the drone reads its graph from Content\\Scripts.");
                    else if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(droneOn
                            ? "A cube driven by Content\\Scripts\\Drone.ocgraph through GraphHost.\nTurning this off releases it."
                            : "Opt-in: spawns a cube and drives it from Content\\Scripts\\Drone.ocgraph through GraphHost.");
                }
                ImGui::Separator();
#endif
                ImGui::BeginDisabled(gameUi_ == nullptr);
                if (ImGui::MenuItem("Game UI Demo", nullptr, showUiDemo_)) showUiDemo_ = !showUiDemo_;
                uiReg_.track("window.gameUiDemo");
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(gameUi_ ? "A hand-written Aver.UI draw list, until there is a widget tree to produce one."
                                              : "The UI render feature is unavailable on this backend.");
                ImGui::Separator();
                // Reset Layout is scoped to whichever tab is in front.
                const bool assetTabActive = !levelVisible_ && assetEditors_.anyOpen();
                if (ImGui::MenuItem(assetTabActive ? "Reset Tab Layout" : "Reset Layout")) {
                    if (assetTabActive) editor::resetActorEditorLayout();
                    else                dockBuilt_ = false;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(assetTabActive
                        ? "Restores this tab's column widths.\nThe editor's panel layout is left alone."
                        : "Restores every panel to its default slot.\nOpen an asset tab to reset that tab instead.");
                ImGui::EndMenu();
            }
            tools_.drawMenu(project_);
            if (ImGui::BeginMenu("Build")){ ImGui::MenuItem("Build Lighting"); ImGui::MenuItem("Build Geometry"); ImGui::EndMenu(); }
            uiReg_.track("menu.build");
            if (ImGui::BeginMenu("Select")){ if(ImGui::MenuItem("Select All")) {} if(ImGui::MenuItem("Select None")) sel_=-1; ImGui::EndMenu(); }
            uiReg_.track("menu.select");
            if (ImGui::BeginMenu("Help")){ ImGui::MenuItem("About Aver Engine"); ImGui::EndMenu(); }
            uiReg_.track("menu.help");
            if (fontMedium_) ImGui::PopFont();
            ImGui::EndMainMenuBar();
        }

        const ImGuiViewport* mv = ImGui::GetMainViewport();
        const ImVec2 wpos = mv->WorkPos, wsize = mv->WorkSize; // already excludes the menu bar
        const f32 toolbarH = 42.0f*dpi_, statusH = 26.0f*dpi_;

        // ---------------- main toolbar ----------------
        ImGui::SetNextWindowPos(wpos);
        ImGui::SetNextWindowSize(ImVec2(wsize.x, toolbarH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##maintoolbar", nullptr, kChromeFlags);
        ImGui::SetCursorPosY((toolbarH - ImGui::GetFrameHeight()) * 0.5f);
        // levelPath_ and saveLevel() are declared only under AVER_MODULE_SCENE (there is no level to
        // path or save without a world), matching the File-menu Save Level item's own guard above --
        // this toolbar button was the same feature, unguarded.
#if AVER_MODULE_SCENE
        ImGui::BeginDisabled(levelPath_.empty());
        if (ImGui::Button("Save")) saveLevel(levelPath_);
        uiReg_.track("toolbar.save");
        ImGui::EndDisabled();
        if (levelPath_.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("No level loaded - File > New Level, then Save Level As");
#else
        ImGui::BeginDisabled(true);
        ImGui::Button("Save");
        uiReg_.track("toolbar.save");
        ImGui::EndDisabled();
#endif
        ImGui::SameLine();
        if (dropButton("Add")) ImGui::OpenPopup("addActor");
        uiReg_.track("toolbar.add");
        if (ImGui::BeginPopup("addActor")) {
            ImGui::TextDisabled("Place Actor"); ImGui::Separator();
            if (ImGui::Selectable("Cube"))     spawnCube(e);
            if (ImGui::Selectable("Player Start")) addPlayerStart(e);
            uiReg_.track("toolbar.add.playerStart");
            ImGui::Selectable("Sphere",  false, ImGuiSelectableFlags_Disabled);
            ImGui::Selectable("Plane",   false, ImGuiSelectableFlags_Disabled);
            ImGui::Selectable("Point Light", false, ImGuiSelectableFlags_Disabled);
            ImGui::EndPopup();
        }
        ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
        tools_.drawCompileButton(project_, dpi_, compileIconUiId_);
        uiReg_.track("toolbar.compileCs");
        ImGui::SameLine();
        // Play controls, centred.
        {
            const f32 grpW = 200.0f*dpi_;
            ImGui::SameLine(std::fmax(ImGui::GetCursorPosX(), (wsize.x - grpW)*0.5f));
#if AVER_MODULE_FRAMEWORK
            const int32_t ps = aver_fw_play_state();
            const bool playing = ps != AVER_FW_PLAY_EDITOR;
            // A drone stand-in is not a play session -- begin_play never ran -- so aver_fw_play_state
            // reports EDITOR throughout. Without folding it in here, Play would stay lit while the
            // drone flew and Stop would sit greyed out, leaving no way to stop it from the toolbar.
            const bool anyPlay = playing || dronePlayActive();
            ImGui::BeginDisabled(anyPlay);
            if (ImGui::Button("Play")) startPlay();
            uiReg_.track("toolbar.play");
            ImGui::EndDisabled();
            ImGui::SameLine();
            // Pause stays tied to a REAL session: there is no framework state to pause for a drone,
            // and a Pause button that visibly does nothing is worse than one that is clearly off.
            ImGui::BeginDisabled(!playing);
            if (ImGui::Button(ps == AVER_FW_PLAY_PAUSED ? "Resume" : "Pause"))
                aver_fw_set_paused(ps != AVER_FW_PLAY_PAUSED ? 1 : 0);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!anyPlay);
            if (ImGui::Button("Stop")) stopPlay();
            uiReg_.track("toolbar.stop");
            ImGui::EndDisabled();
#else
            ImGui::BeginDisabled(true);
            ImGui::Button("Play"); ImGui::SameLine();
            ImGui::Button("Pause"); ImGui::SameLine();
            ImGui::Button("Stop");
            ImGui::EndDisabled();
#endif
        }
        ImGui::SameLine(std::fmax(ImGui::GetCursorPosX(), wsize.x - 130.0f*dpi_));
        if (dropButton("Settings")) ImGui::OpenPopup("settingsMenu");
        if (ImGui::BeginPopup("settingsMenu")) {
            ImGui::Checkbox("Show Grid", &showGrid_);
            ImGui::Checkbox("Wireframe", &wireframe_);
            ImGui::EndPopup();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);

        // ---------------- dockspace host ----------------
        ImGui::SetNextWindowPos(ImVec2(wpos.x, wpos.y + toolbarH));
        ImGui::SetNextWindowSize(ImVec2(wsize.x, wsize.y - toolbarH - statusH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
        ImGui::Begin("##dockhost", nullptr, kChromeFlags | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar(3);

        const ImGuiID dockId = ImGui::GetID("AverDockspace");
        const ImVec2 dockSize(wsize.x, wsize.y - toolbarH - statusH);
        ImGui::DockSpace(dockId, ImVec2(0,0), ImGuiDockNodeFlags_PassthruCentralNode);

        if (!dockBuilt_ && dockSize.x > 1.0f && dockSize.y > 1.0f) {
            dockBuilt_ = true;
            ImGui::DockBuilderRemoveNode(dockId);
            ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace); // private flag, required here
            ImGui::DockBuilderSetNodeSize(dockId, dockSize); // must precede the splits
            ImGuiID centre = dockId, right = 0, rightTop = 0, rightBottom = 0;
            ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.22f, &right,  &centre);
            ImGui::DockBuilderSplitNode(right,  ImGuiDir_Down,  0.60f, &rightBottom, &rightTop);
            ImGui::DockBuilderDockWindow("World Outliner",  rightTop);
            ImGui::DockBuilderDockWindow("Details",         rightBottom);
            ImGui::DockBuilderDockWindow("Level",           centre);
            ImGui::DockBuilderFinish(dockId);
        }
        if (const ImGuiDockNode* cn = ImGui::DockBuilderGetCentralNode(dockId)) centralDock_ = cn->ID;
        ImGui::End(); // ##dockhost

        // ---------------- the level, as a tab drawing the scene texture ----------------
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            if (focusLevelAt_ > 0 && ImGui::GetFrameCount() == focusLevelAt_) {
                ImGui::SetWindowFocus("Level");
                AVER_INFO("[Editor] --focus-level-at: bringing the Level tab forward");
            }
            levelVisible_ = ImGui::Begin("Level", nullptr,
                                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                         ImGuiWindowFlags_NoCollapse);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const f32 w = avail.x > 8.0f ? avail.x : 8.0f;
            const f32 h = avail.y > 8.0f ? avail.y : 8.0f;

            vpX_ = at.x; vpY_ = at.y; vpW_ = w; vpH_ = h;
            levelFocused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            levelHovered_ = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                                                   ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

            if (levelVisible_ && e.device()) {
                e.device()->setViewportToTexture(true);
                if (const u64 tex = e.device()->viewportTextureId()) {
                    // The texture is the whole backbuffer; the scene is this sub-rect of it.
                    const f32 bw = ImGui::GetIO().DisplaySize.x;
                    const f32 bh = ImGui::GetIO().DisplaySize.y;
                    if (bw > 1.0f && bh > 1.0f) {
                        const ImVec2 uv0(at.x / bw, at.y / bh);
                        const ImVec2 uv1((at.x + w) / bw, (at.y + h) / bh);
                        ImGui::Image(static_cast<ImTextureID>(tex), ImVec2(w, h), uv0, uv1);
                        // Drop target for content-browser assets (see kAssetDragDropType). The scene
                        // symbols the actual placement needs live in spawnFromAssetDrop, guarded on
                        // their own -- this call site stays compilable with the module off either way.
                        if (ImGui::BeginDragDropTarget()) {
                            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetDragDropType)) {
                                const std::string droppedPath(
                                    static_cast<const char*>(payload->Data),
                                    payload->DataSize > 0 ? static_cast<usize>(payload->DataSize - 1) : usize(0));
                                const ImVec2 mp = ImGui::GetMousePos();
#if AVER_MODULE_SCENE
                                spawnFromAssetDrop(e, droppedPath, mp.x, mp.y);
#else
                                cbStatus_ = "Placing objects needs the scene module";
                                AVER_WARN("[Editor] drop: scene module not compiled in, ignoring '{}'", droppedPath);
#endif
                            }
                            ImGui::EndDragDropTarget();
                        }
                    }
                }
            }
            ImGui::End();
            ImGui::PopStyleVar();
        }

        if (levelVisible_ || !assetEditors_.anyOpen()) buildPanels(e);
        if (levelVisible_) buildViewportOverlay();
        drawDrawer(e);
        buildEditorPrefs();
        buildProjectSettings();
        buildWorldSettings();
#if AVER_MODULE_SCENE
#if AVER_WITH_IMGUI
        buildChunkStreamingPanel();
#endif
#endif
        if (!openAsset_.empty() && frameNo_ > 5) {
            const std::string want = openAsset_;
            openAsset_.clear();
            lastOpenAssetPath_ = want; // --graph-select (below) needs this after openAsset_ is cleared
            if (assetEditors_.open(want)) AVER_INFO("[Editor] --open-asset opened {}", want);
            else AVER_ERROR("[Editor] --open-asset: no registered editor accepts {}", want);
        }
        // --graph-select <nodeId>: one frame after the block above could have opened something, so the
        // editor this looks for is guaranteed to exist by the time this runs. See
        // setGraphSelectNode's own comment for why this exists at all.
        if (!graphSelectNode_.empty() && frameNo_ > 6) {
            const std::string nodeId = graphSelectNode_;
            graphSelectNode_.clear();
            if (auto* ed = assetEditors_.find(lastOpenAssetPath_)) {
                if (auto* ge = dynamic_cast<editor::GraphEditor*>(ed)) {
                    // The message follows the RESULT, not the call. It used to say "selected node"
                    // unconditionally, which is how a capture run against a node id that does not
                    // exist reported success and proved nothing.
                    if (ge->selectNode(nodeId))
                        AVER_INFO("[Editor] --graph-select selected '{}'", nodeId);
                    else
                        AVER_ERROR("[Editor] --graph-select: '{}' names no node or component in {}",
                                   nodeId, lastOpenAssetPath_);
                } else {
                    AVER_ERROR("[Editor] --graph-select: '{}' is not a graph editor", lastOpenAssetPath_);
                }
            } else {
                AVER_ERROR("[Editor] --graph-select: no open editor for '{}'", lastOpenAssetPath_);
            }
        }
        // --graph-tab <name>: same one-frame-after-open timing as --graph-select above, and the
        // same dynamic_cast, because the same thing is true -- only a graph editor has inner tabs.
        if (!graphTab_.empty() && frameNo_ > 6) {
            const std::string tab = graphTab_;
            graphTab_.clear();
            if (auto* ed = assetEditors_.find(lastOpenAssetPath_)) {
                if (auto* ge = dynamic_cast<editor::GraphEditor*>(ed)) {
                    if (tab == "viewport") { ge->showViewportTab(); AVER_INFO("[Editor] --graph-tab viewport"); }
                    else AVER_WARN("[Editor] --graph-tab: only 'viewport' is selectable, got '{}'", tab);
                } else {
                    AVER_ERROR("[Editor] --graph-tab: '{}' is not a graph editor", lastOpenAssetPath_);
                }
            } else {
                AVER_ERROR("[Editor] --graph-tab: no open editor for '{}'", lastOpenAssetPath_);
            }
        }
        pumpContentWatch();
        assetEditors_.draw(e, centralDock_, dpi_);
        tools_.drawModals(project_, dpi_);
        drawUpgradePrompt();
        drawExitPrompt(e);

        // ---------------- status bar ----------------
        ImGui::SetNextWindowPos(ImVec2(wpos.x, wpos.y + wsize.y - statusH));
        ImGui::SetNextWindowSize(ImVec2(wsize.x, statusH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##statusbar", nullptr, kChromeFlags);
        const f32 dt = e.time().dt;
        ImGui::SetCursorPosY((statusH - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::Text("%s  |  %s  |  %s  |  DPI %.0f%%  |  %.0f FPS (%.2f ms)  |  %zu actors  |  %s",
                    project_.valid() ? project_.name.c_str() : "No project",
                    rhi::backendName(e.device()->backend()), e.device()->adapterName(), dpi_*100.f,
                    dt>1e-6f?1.f/dt:0.f, dt*1000.f, objects_.size(),
                    selectionLabel().c_str());

        // The upgrade outcome, for a while. Twelve seconds is long enough to read after clicking a
        // button and short enough that it does not become permanent furniture. A FAILURE is drawn in
        // the error colour, because "Upgrade failed: ..." sliding past in the same grey as the frame
        // rate is how a user concludes it worked.
        if (!upgradeStatus_.empty()) {
            upgradeStatusAge_ += dt;
            if (upgradeStatusAge_ > 12.0f) {
                upgradeStatus_.clear();
            } else {
                ImGui::SameLine();
                ImGui::TextUnformatted("  |  ");
                ImGui::SameLine();
                const bool bad = upgradeStatus_.rfind("Upgrade failed", 0) == 0;
                ImGui::TextColored(bad ? ImVec4(1.0f, 0.45f, 0.40f, 1.0f)
                                       : ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                                   "%s", upgradeStatus_.c_str());
            }
        }

        auto drawerButton = [&](const char* label, Drawer d, const char* tip) {
            const bool on = drawer_ == d;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::SmallButton(label)) toggleDrawer(d);
            if (on) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        };
        ImGui::SameLine(std::fmax(ImGui::GetCursorPosX(), wsize.x - 250.0f*dpi_));
        drawerButton("Content Browser", Drawer::Content, "Show the Content Browser  (Ctrl+Space)");
        ImGui::SameLine();
        drawerButton("Output Log", Drawer::Log, "Show the Output Log");
        ImGui::End();
        ImGui::PopStyleVar(2);
#else
        (void)e;
#endif
    }

#if AVER_WITH_IMGUI
    // Draws the Output Log: clear / level filter / auto-scroll over a colour-coded view of the
    // captured log. Reads the shared buffer under logMutex_.
    void drawOutputLog() {
        if (ImGui::SmallButton("Clear")) { std::lock_guard<std::mutex> lk(logMutex_); logLines_.clear(); }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f * dpi_);
        ImGui::Combo("##loglevel", &logLevelFilter_, "All\0Info+\0Warn+\0");
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &logAutoScroll_);
        ImGui::Separator();

        ImGui::BeginChild("##logscroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::lock_guard<std::mutex> lk(logMutex_);
            const int minLevel = logLevelFilter_ == 2 ? (int)LogLevel::Warn
                              : logLevelFilter_ == 1 ? (int)LogLevel::Info : (int)LogLevel::Trace;
            for (const LogLine& ln : logLines_) {
                if ((int)ln.level < minLevel) continue;
                ImVec4 col;
                switch (ln.level) {
                    case LogLevel::Error: col = ImVec4(0.95f, 0.40f, 0.38f, 1.0f); break;
                    case LogLevel::Warn:  col = ImVec4(0.95f, 0.78f, 0.35f, 1.0f); break;
                    case LogLevel::Trace: col = ImVec4(0.55f, 0.57f, 0.62f, 1.0f); break;
                    default:              col = ImVec4(0.82f, 0.84f, 0.88f, 1.0f); break;
                }
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextUnformatted(ln.text.c_str());
                ImGui::PopStyleColor();
            }
        }
        if (logAutoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }

    // One root the Content Browser mounts.
    struct CbRoot { const char* label; std::string path; bool engine; };
    // Returns the mounted roots: the project's Content, and the engine's source tree where present.
    std::vector<CbRoot> cbRoots() const {
        std::vector<CbRoot> r;
        if (project_.valid()) r.push_back({"Content", project_.contentDir(), false});
        if (!cbEngineRoot_.empty()) r.push_back({"Engine", cbEngineRoot_, true});
        return r;
    }

    // Enters a folder and records it in the Back/Forward history.
    void cbNavigate(const std::string& dir) {
        if (dir.empty() || dir == cbSelectedDir_) return;
        if (cbHistoryPos_ >= 0 && cbHistoryPos_ + 1 < static_cast<int>(cbHistory_.size()))
            cbHistory_.resize(static_cast<usize>(cbHistoryPos_) + 1);
        cbHistory_.push_back(dir);
        cbHistoryPos_ = static_cast<int>(cbHistory_.size()) - 1;
        cbSelectedDir_ = dir;
        cbSelectedFile_.clear();
        cbFilter_[0] = '\0';
    }

    bool cbCanBack()    const { return cbHistoryPos_ > 0; }
    bool cbCanForward() const { return cbHistoryPos_ >= 0 &&
                                       cbHistoryPos_ + 1 < static_cast<int>(cbHistory_.size()); }
    void cbBack()    { if (cbCanBack())    { cbSelectedDir_ = cbHistory_[static_cast<usize>(--cbHistoryPos_)]; cbSelectedFile_.clear(); } }
    void cbForward() { if (cbCanForward()) { cbSelectedDir_ = cbHistory_[static_cast<usize>(++cbHistoryPos_)]; cbSelectedFile_.clear(); } }

    // Returns the parent folder, or empty at a mounted root.
    std::string cbParentDir() const {
        for (const CbRoot& r : cbRoots())
            if (cbSelectedDir_ == r.path) return {};
        std::error_code ec;
        std::filesystem::path p = std::filesystem::path(cbSelectedDir_).parent_path();
        if (p.empty()) return {};
        const std::string up = p.string();
        for (const CbRoot& r : cbRoots())
            if (up.size() >= r.path.size() && up.compare(0, r.path.size(), r.path) == 0) return up;
        return {};
    }

    // True for extensions a double-click should hand to the IDE rather than the shell.
    static bool cbIsSourceFile(const std::string& ext) {
        static const char* kSource[] = {
            ".cs", ".cpp", ".cxx", ".cc", ".c", ".hpp", ".hxx", ".h", ".inl",
            ".hlsl", ".hlsli", ".glsl", ".json", ".xml", ".csproj", ".txt", ".md", ".ini", ".cmake"};
        for (const char* s : kSource) if (ext == s) return true;
        return false;
    }

    // Returns the IDE the browser opens source with: the user's choice, else the detected preference.
    // STAYS INSIDE the UI block with the rest of the content browser. An earlier attempt lifted it
    // out because it touches no ImGui itself -- true, and beside the point: its neighbours
    // cbIsEditable/cbInvalidate/importModel do, so lifting one helper only moved the undefined
    // identifier one call deeper. The callers are guarded instead.
    const editor::IdeInfo& cbIde() const {
        const std::vector<editor::IdeInfo>& ides = editor::detectedIdes();
        if (cbIdeChoice_ >= 0 && cbIdeChoice_ < static_cast<int>(ides.size())) return ides[static_cast<usize>(cbIdeChoice_)];
        return editor::preferredIde();
    }

    // Handles a double-click: enter a folder, open an asset editor, else the IDE, else the shell.
    void cbOpenEntry(const std::string& full, bool isDir) {
        if (isDir) { cbNavigate(full); return; }
        const std::string ext = lowerExt(std::filesystem::path(full));
        if (assetEditors_.open(full)) { cbStatus_ = "Opened in the asset editor"; return; }
        if (cbIsSourceFile(ext)) {
            const editor::IdeInfo& ide = cbIde();
            if (editor::openInIde(ide, full)) { cbStatus_ = "Opened in " + ide.name; return; }
            cbStatus_ = "Could not open in " + ide.name;
            return;
        }
        cbStatus_ = editor::openWithShell(full) ? "Opened" : "Nothing is registered to open that";
    }

    // Drops a folder's cached listing so the next frame re-reads it.
    void cbInvalidate(const std::string& dir) { dirCache_.erase(dir); }

    // False for engine content, which the browser mounts read-only.
    bool cbIsEditable(const std::string& path) const { return !isEnginePath(path); }

    // Explains why `dir` cannot be imported/created into, or empty if it can.
    std::string cbImportBlockedReason(const std::string& dir) const {
        if (cbIsEditable(dir)) return {};
        const std::string name = std::filesystem::path(dir).filename().string();
        return "'" + (name.empty() ? dir : name) + "' is engine content and is read-only.";
    }

    // Renames a file or folder and follows the rename in the selection and the history.
    void cbRenameEntry(const std::string& from, const std::string& newName) {
        if (newName.empty()) return;
        std::error_code ec;
        const std::filesystem::path src(from);
        const std::filesystem::path dst = src.parent_path() / newName;
        if (std::filesystem::exists(dst, ec)) { cbStatus_ = "'" + newName + "' already exists"; return; }
        std::filesystem::rename(src, dst, ec);
        if (ec) { cbStatus_ = "Rename failed: " + ec.message(); return; }
        cbInvalidate(src.parent_path().string());
        if (cbSelectedDir_ == from)  { cbSelectedDir_ = dst.string(); }
        if (cbSelectedFile_ == from) { cbSelectedFile_ = dst.string(); }
        cbRewriteHistory(from, dst.string());
        cbStatus_ = "Renamed to " + newName;
    }

    // Rewrites history entries under `from` to `to`, dropping them when `to` is empty.
    void cbRewriteHistory(const std::string& from, const std::string& to) {
        std::vector<std::string> kept;
        kept.reserve(cbHistory_.size());
        const std::string current = cbHistoryPos_ >= 0 && cbHistoryPos_ < static_cast<int>(cbHistory_.size())
                                  ? cbHistory_[static_cast<usize>(cbHistoryPos_)] : std::string();
        std::string newCurrent = current;
        for (const std::string& h : cbHistory_) {
            const bool under = h.size() >= from.size() && h.compare(0, from.size(), from) == 0 &&
                               (h.size() == from.size() || h[from.size()] == '\\' || h[from.size()] == '/');
            std::string next = h;
            if (under) {
                if (to.empty()) { if (h == current) newCurrent.clear(); continue; }
                next = to + h.substr(from.size());
            }
            if (h == current) newCurrent = next;
            if (kept.empty() || kept.back() != next) kept.push_back(next);
        }
        cbHistory_.swap(kept);
        cbHistoryPos_ = -1;
        for (usize i = 0; i < cbHistory_.size(); ++i)
            if (cbHistory_[i] == newCurrent) { cbHistoryPos_ = static_cast<int>(i); break; }
        if (cbHistoryPos_ < 0 && !cbHistory_.empty()) cbHistoryPos_ = static_cast<int>(cbHistory_.size()) - 1;
    }

    // Copies a file or folder alongside itself as "<name>2", "<name>3", ...
    void cbDuplicateEntry(const std::string& path) {
        std::error_code ec;
        const std::filesystem::path src(path);
        const std::string stem = src.stem().string(), ext = src.extension().string();
        std::filesystem::path dst;
        for (int n = 2; n < 1000; ++n) {
            dst = src.parent_path() / (stem + std::to_string(n) + ext);
            if (!std::filesystem::exists(dst, ec)) break;
        }
        if (std::filesystem::is_directory(src, ec))
            std::filesystem::copy(src, dst, std::filesystem::copy_options::recursive, ec);
        else
            std::filesystem::copy_file(src, dst, ec);
        if (ec) { cbStatus_ = "Duplicate failed: " + ec.message(); return; }
        cbInvalidate(src.parent_path().string());
        cbStatus_ = "Duplicated as " + dst.filename().string();
    }

    // Moves a file or folder to the recycle bin and drops it from the selection and history.
    void cbDeleteEntry(const std::string& path) {
        const std::filesystem::path src(path);
        const std::string parent = src.parent_path().string();
        if (!editor::moveToRecycleBin(path)) { cbStatus_ = "Could not delete " + src.filename().string(); return; }
        cbInvalidate(parent);
        if (cbSelectedFile_ == path) cbSelectedFile_.clear();
        if (cbSelectedDir_ == path) cbSelectedDir_ = parent;
        cbRewriteHistory(path, std::string());
        cbStatus_ = "Moved " + src.filename().string() + " to the recycle bin";
    }

    // Creates a folder under parent. Refuses engine content.
    void cbCreateFolder(const std::string& parent, const std::string& name) {
        if (name.empty()) return;
        if (!cbIsEditable(parent)) { cbStatus_ = "Engine content is read-only"; return; }
        std::error_code ec;
        const std::filesystem::path dst = std::filesystem::path(parent) / name;
        if (std::filesystem::exists(dst, ec)) { cbStatus_ = "'" + name + "' already exists"; return; }
        std::filesystem::create_directory(dst, ec);
        if (ec) { cbStatus_ = "Could not create folder: " + ec.message(); return; }
        cbInvalidate(parent);
        cbStatus_ = "Created " + name;
    }

    // Draws the right-click menu for one Content Browser entry.
    void cbItemContextMenu(const std::string& full, const std::string& name, bool isDir) {
        if (!ImGui::BeginPopupContextItem("##cbitemctx")) return;
        const bool editable = cbIsEditable(full);
        ImGui::TextDisabled("%s", name.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem(isDir ? "Open" : "Open in editor", "Double-click")) cbOpenEntry(full, isDir);
        if (!isDir) {
            if (ImGui::BeginMenu("Open With")) {
                for (usize i = 0; i < editor::detectedIdes().size(); ++i) {
                    const editor::IdeInfo& ide = editor::detectedIdes()[i];
                    if (ImGui::MenuItem(ide.name.c_str()))
                        cbStatus_ = editor::openInIde(ide, full) ? "Opened in " + ide.name
                                                                : "Could not open in " + ide.name;
                }
                ImGui::EndMenu();
            }
        }
        if (ImGui::MenuItem("Show in Explorer")) editor::revealInFileManager(full);
        if (ImGui::MenuItem("Copy Path")) { ImGui::SetClipboardText(full.c_str()); cbStatus_ = "Path copied"; }
        ImGui::Separator();
        ImGui::BeginDisabled(!editable);
        if (ImGui::MenuItem("Rename", "F2")) {
            cbContextPath_ = full; cbContextIsDir_ = isDir; cbWantRename_ = true;
            std::snprintf(cbRenameBuf_, sizeof cbRenameBuf_, "%s", name.c_str());
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) { cbContextPath_ = full; cbContextIsDir_ = isDir; cbWantDuplicate_ = true; }
        if (ImGui::MenuItem("Delete", "Del")) { cbContextPath_ = full; cbContextIsDir_ = isDir; cbWantDelete_ = true; }
        ImGui::EndDisabled();
        if (!editable) ImGui::TextDisabled("Engine content is read-only here.");
        ImGui::EndPopup();
    }

    // True for a path inside the Engine root -- what earns the Module folder icon.
    bool isEnginePath(const std::string& p) const {
        return !cbEngineRoot_.empty() && p.size() >= cbEngineRoot_.size() &&
               p.compare(0, cbEngineRoot_.size(), cbEngineRoot_) == 0;
    }

    // Draws the Content Browser: navigation, Add and Import, and a folder tree beside a file view.
    void drawContentBrowser() {
        const std::vector<CbRoot> roots = cbRoots();
        if (roots.empty()) {
            ImGui::TextDisabled("No project loaded - nothing is mounted.");
            ImGui::TextDisabled("Create or open a project (File menu) to browse its Content folder.");
            return;
        }
        if (cbSelectedDir_.empty()) cbNavigate(roots.front().path);
        if (!drawerStartSub_.empty()) {
            // Canonicalised so separators and case match what the tree builds from directory_iterator.
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path target = fs::canonical(fs::path(roots.front().path) / drawerStartSub_, ec);
            if (ec) AVER_WARN("[Sandbox] --drawer content:{}: no such folder under Content", drawerStartSub_);
            else    cbNavigate(target.string());
            drawerStartSub_.clear();
        }
        ImGui::BeginDisabled(!cbCanBack());
        if (ImGui::ArrowButton("##cbback", ImGuiDir_Left)) cbBack();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Back");
        ImGui::SameLine(0.0f, 2.0f*dpi_);
        ImGui::BeginDisabled(!cbCanForward());
        if (ImGui::ArrowButton("##cbfwd", ImGuiDir_Right)) cbForward();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Forward");
        ImGui::SameLine(0.0f, 2.0f*dpi_);
        const std::string parent = cbParentDir();
        ImGui::BeginDisabled(parent.empty());
        if (ImGui::ArrowButton("##cbup", ImGuiDir_Up)) cbNavigate(parent);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(parent.empty() ? "Already at the root" : "Up one folder");
        ImGui::SameLine();

        if (ImGui::Button("+ Add")) ImGui::OpenPopup("cbAddMenu");
        if (ImGui::BeginPopup("cbAddMenu")) {
            ImGui::BeginDisabled(!cbIsEditable(cbSelectedDir_));
            if (ImGui::MenuItem("New Folder")) { cbWantNewFolder_ = true; cbNewFolderBuf_[0] = '\0'; }
            ImGui::EndDisabled();
            ImGui::Separator();
            if (ImGui::MenuItem("New C# Script...")) tools_.openNewCsScript();
            if (ImGui::MenuItem("New C# Class..."))  tools_.openNewCsClass();
            ImGui::Separator();
            if (ImGui::MenuItem("New C++ Module...")) tools_.openNewCppModule();
            if (ImGui::MenuItem("New C++ Class..."))  tools_.openNewCppClass();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        {
            const std::string reason = cbImportBlockedReason(cbSelectedDir_);
            ImGui::BeginDisabled(!reason.empty());
            if (ImGui::Button("Import...")) cbWantImport_ = true;
            ImGui::EndDisabled();
            if (!reason.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", reason.c_str());
        }
        drawImportModal();

        // View controls, right-aligned: Tiles/List, and the tile zoom when tiles are showing.
        {
            auto viewTab = [&](const char* label, bool active) {
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                const bool hit = ImGui::Button(label);
                if (active) ImGui::PopStyleColor();
                return hit;
            };
            const f32 controls = cbGallery_ ? 250.0f : 130.0f;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(std::fmax(80.0f*dpi_,
                ImGui::GetWindowWidth() - ImGui::GetCursorPosX() - (controls + 14.0f)*dpi_));
            ImGui::InputTextWithHint("##cbsearch", "Search this folder...", cbFilter_, sizeof(cbFilter_));

            ImGui::SameLine(std::fmax(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - controls*dpi_));
            if (viewTab("Tiles", cbGallery_)) cbGallery_ = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Gallery view");
            ImGui::SameLine(0.0f, 2.0f*dpi_);
            if (viewTab("List", !cbGallery_)) cbGallery_ = false;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("List view");
            if (cbGallery_) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(110.0f*dpi_);
                ImGui::SliderFloat("##cbzoom", &cbTileSize_, 56.0f, 168.0f, "%.0f");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tile size");
            }
        }
        ImGui::Separator();

        const f32 footerH = ImGui::GetTextLineHeightWithSpacing() + 6.0f*dpi_;
        ImGui::BeginChild("cbTree", ImVec2(220.0f * dpi_, -footerH), true);
        for (const CbRoot& r : roots) {
            ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_SpanAvailWidth;
            if (!r.engine) rootFlags |= ImGuiTreeNodeFlags_DefaultOpen;
            if (cbSelectedDir_ == r.path) rootFlags |= ImGuiTreeNodeFlags_Selected;
            ImGui::PushID(r.label);
            const bool open = ImGui::TreeNodeEx(r.label, rootFlags);
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) cbNavigate(r.path);
            if (open) { drawFolderTree(r.path); ImGui::TreePop(); }
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("cbFiles", ImVec2(0, -footerH), true);
        drawFolderFiles(cbSelectedDir_);
        if (ImGui::BeginPopupContextWindow("##cbbgctx",
                ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            const std::string reason = cbImportBlockedReason(cbSelectedDir_);
            ImGui::BeginDisabled(!reason.empty());
            if (ImGui::MenuItem("New Folder")) { cbWantNewFolder_ = true; cbNewFolderBuf_[0] = '\0'; }
            if (ImGui::MenuItem("Import...")) cbWantImport_ = true;
            ImGui::EndDisabled();
            if (!reason.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", reason.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Show in Explorer")) editor::revealInFileManager(cbSelectedDir_);
            if (ImGui::MenuItem("Copy Path")) { ImGui::SetClipboardText(cbSelectedDir_.c_str()); cbStatus_ = "Path copied"; }
            if (ImGui::MenuItem("Refresh")) cbInvalidate(cbSelectedDir_);
            ImGui::EndPopup();
        }
        ImGui::EndChild();

        cbFooter();
        cbShortcuts();
        cbFileOpModals();
    }

    // Draws the browser footer: the folder's counts, the selection, and the last operation's outcome.
    void cbFooter() {
        const DirListing& l = dirListing(cbSelectedDir_);
        const usize files = l.entries.size() - l.dirCount;
        ImGui::Text("%zu folder%s, %zu file%s", l.dirCount, l.dirCount == 1 ? "" : "s",
                    files, files == 1 ? "" : "s");
        if (!cbSelectedFile_.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("|  %s", std::filesystem::path(cbSelectedFile_).filename().string().c_str());
        }
        if (!cbStatus_.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("|  %s", cbStatus_.c_str());
        }
    }

    // Runs Enter / F2 / Delete / Ctrl+D on the browser selection, suppressed while a popup is up.
    void cbShortcuts() {
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
        if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput || io.WantCaptureKeyboard || cbSelectedFile_.empty()) return;
        std::error_code ec;
        const bool isDir = std::filesystem::is_directory(cbSelectedFile_, ec);
        const bool editable = cbIsEditable(cbSelectedFile_);
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) cbOpenEntry(cbSelectedFile_, isDir);
        if (editable && ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
            cbContextPath_ = cbSelectedFile_; cbContextIsDir_ = isDir; cbWantRename_ = true;
            std::snprintf(cbRenameBuf_, sizeof cbRenameBuf_, "%s",
                          std::filesystem::path(cbSelectedFile_).filename().string().c_str());
        }
        if (editable && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            cbContextPath_ = cbSelectedFile_; cbContextIsDir_ = isDir; cbWantDelete_ = true;
        }
        if (editable && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) {
            cbContextPath_ = cbSelectedFile_; cbContextIsDir_ = isDir; cbWantDuplicate_ = true;
        }
    }

    // Runs the deferred file operations and their dialogs: duplicate, rename, delete, new folder, import.
    void cbFileOpModals() {
        if (cbWantDuplicate_) { cbWantDuplicate_ = false; cbDuplicateEntry(cbContextPath_); }

        if (cbWantRename_)    { ImGui::OpenPopup("cbRename");    cbWantRename_ = false; }
        if (cbWantDelete_)    { ImGui::OpenPopup("cbDelete");    cbWantDelete_ = false; }
        if (cbWantNewFolder_) { ImGui::OpenPopup("cbNewFolder"); cbWantNewFolder_ = false; }
        if (cbWantImport_)    { ImGui::OpenPopup("Import Asset"); cbWantImport_ = false; }

        if (ImGui::BeginPopupModal("cbRename", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextDisabled("Rename %s", cbContextIsDir_ ? "folder" : "file");
            ImGui::SetNextItemWidth(360.0f*dpi_);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            const bool submit = ImGui::InputText("##cbrenametxt", cbRenameBuf_, sizeof cbRenameBuf_,
                                                 ImGuiInputTextFlags_EnterReturnsTrue);
            const bool valid = cbRenameBuf_[0] != '\0' && !std::strpbrk(cbRenameBuf_, "\\/:*?\"<>|");
            if (!valid && cbRenameBuf_[0] != '\0') ImGui::TextColored(ImVec4(0.95f,0.5f,0.45f,1), "That name is not a legal filename.");
            ImGui::BeginDisabled(!valid);
            if (ImGui::Button("Rename") || (submit && valid)) {
                cbRenameEntry(cbContextPath_, cbRenameBuf_);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("cbDelete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(cbContextIsDir_
                ? "Delete this folder and everything in it?"
                : "Delete this file?");
            ImGui::TextDisabled("%s", cbContextPath_.c_str());
            ImGui::TextDisabled("It goes to the recycle bin, so it can be restored.");
            if (ImGui::Button("Delete")) { cbDeleteEntry(cbContextPath_); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("cbNewFolder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextDisabled("New folder in %s", cbSelectedDir_.c_str());
            ImGui::SetNextItemWidth(360.0f*dpi_);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            const bool submit = ImGui::InputText("##cbnewfoldertxt", cbNewFolderBuf_, sizeof cbNewFolderBuf_,
                                                 ImGuiInputTextFlags_EnterReturnsTrue);
            const bool valid = cbNewFolderBuf_[0] != '\0' && !std::strpbrk(cbNewFolderBuf_, "\\/:*?\"<>|");
            ImGui::BeginDisabled(!valid);
            if (ImGui::Button("Create") || (submit && valid)) {
                cbCreateFolder(cbSelectedDir_, cbNewFolderBuf_);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    // Draws the selected folder as clickable ancestor segments.
    void drawBreadcrumb(const std::string& dir) {
        const std::vector<CbRoot> roots = cbRoots();
        const CbRoot* owner = nullptr;
        for (const CbRoot& r : roots)
            if (dir.size() >= r.path.size() && dir.compare(0, r.path.size(), r.path) == 0) { owner = &r; break; }
        if (!owner) { ImGui::TextDisabled("%s", dir.c_str()); return; }

        std::string acc = owner->path;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f*dpi_, 1.0f*dpi_));
        if (ImGui::SmallButton(owner->label)) cbNavigate(acc);
        std::string tail = dir.substr(owner->path.size());
        usize i = 0;
        while (i < tail.size()) {
            while (i < tail.size() && (tail[i] == '\\' || tail[i] == '/')) ++i;
            const usize start = i;
            while (i < tail.size() && tail[i] != '\\' && tail[i] != '/') ++i;
            if (i == start) break;
            const std::string seg = tail.substr(start, i - start);
            acc += "\\" + seg;
            ImGui::SameLine(0.0f, 2.0f*dpi_); ImGui::TextDisabled(">"); ImGui::SameLine(0.0f, 2.0f*dpi_);
            ImGui::PushID(static_cast<int>(start));
            if (ImGui::SmallButton(seg.c_str())) cbNavigate(acc);
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
    }

    // Returns a directory's sorted listing, cached for 20 frames.
    const DirListing& dirListing(const std::string& dir) {
        DirListing& c = dirCache_[dir];
        if (frameNo_ - c.stamp < 20) return c;
        c.stamp = frameNo_;
        c.entries.clear(); c.dirCount = 0;
        std::error_code ec;
        try {
            for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code fec;
                DirEntry ent;
                ent.path  = it->path();
                ent.isDir = it->is_directory(fec);
                ent.full  = ent.path.string();
                ent.name  = ent.path.filename().string();
                if (ent.isDir) {
                    std::error_code mec;
                    ent.module = isEnginePath(ent.full) ||
                                 std::filesystem::exists(ent.path / "CMakeLists.txt", mec);
                    ++c.dirCount;
                } else {
                    ent.tile = fileIconTile(ent.full, ent.name, lowerExt(ent.path));
                }
                c.entries.push_back(std::move(ent));
            }
        } catch (const std::exception&) { }
        // Folders first, then files, each alphabetical: the views split the groups by dirCount alone.
        std::sort(c.entries.begin(), c.entries.end(), [](const DirEntry& a, const DirEntry& b) {
            if (a.isDir != b.isDir) return a.isDir;
            return a.full < b.full;
        });
        return c;
    }

    // Draws a folder's subfolders as tree nodes, recursing into the ones that are open.
    void drawFolderTree(const std::string& dir) {
        std::vector<std::pair<std::string, std::string>> subs;   // (full, name)
        {
            const DirListing& l = dirListing(dir);
            subs.reserve(l.dirCount);
            for (const DirEntry& e : l.entries) { if (!e.isDir) break; subs.emplace_back(e.full, e.name); }
        }
        for (const auto& [full, name] : subs) {
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (cbSelectedDir_ == full) flags |= ImGuiTreeNodeFlags_Selected;
            const bool open = ImGui::TreeNodeEx(name.c_str(), flags);
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) cbNavigate(full);
            if (open) { drawFolderTree(full); ImGui::TreePop(); }
        }
    }

    // Returns an ASSET sheet tile for an engine asset extension, or -1. Separate from the source-file
    // sheet because the two are different textures with different provenance -- see branding/ASSETS.md.
    static int assetIconTile(const std::string& ext) {
        if (ext == ".ocanim") return 0;
        if (ext == ".ocskel") return 1;
        if (ext == ".ocmesh") return 2;
        if (ext == ".ocgraph") return 3;
        return -1;
    }

    // Returns a file's sprite tile. 0..3 index the file sheet (C# Script / C# Class / C++ Class /
    // C++ Module); kAssetTileBase + n indexes the asset sheet; -1 is neither. A .cs is classified by
    // peeking at its head and cached against the file's modification time.
    //
    // THE ASSET CHECK COMES FIRST, and has to. Below it sits `if (ext != ".cs") return -1;`, so an
    // extension arm added after that line compiles, reads correctly, and never runs.
    int fileIconTile(const std::string& path, const std::string& name, const std::string& ext) {
        if (const int a = assetIconTile(ext); a >= 0) return kAssetTileBase + a;
        if (name == "CMakeLists.txt") return 3;
        if (ext == ".cpp" || ext == ".cxx" || ext == ".cc" || ext == ".hpp" || ext == ".hxx" || ext == ".h")
            return 2;
        if (ext != ".cs") return -1;
        std::error_code ec;
        const auto mtime = std::filesystem::last_write_time(path, ec);
        if (auto it = fileIconCache_.find(path);
            it != fileIconCache_.end() && !ec && it->second.first == mtime) return it->second.second;

        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return 1;
        int tile = 1;
        char head[8192];
        in.read(head, sizeof(head));
        const std::string body(head, static_cast<size_t>(in.gcount()));
        static const char* kScriptMarkers[] = {
            "AverBehaviour", "AverActor", "AverPawn", "AverCharacter", "AverPlayerController",
            "AverGameMode", "AverGameInstance", "[AverClass", "[AverGameMode"};
        for (const char* m : kScriptMarkers) if (body.find(m) != std::string::npos) { tile = 0; break; }
        if (!ec) fileIconCache_[path] = {mtime, tile};
        return tile;
    }

    // Draws a folder icon: a body with a raised tab.
    static void folderGlyph(ImDrawList* dl, ImVec2 c, f32 s, ImU32 col) {
        const f32 w = s, h = s * 0.76f;
        const f32 x0 = c.x - w*0.5f, y0 = c.y - h*0.5f, tabH = h * 0.17f;
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w*0.44f, y0 + tabH*2.4f), col, s*0.06f);
        dl->AddRectFilled(ImVec2(x0, y0 + tabH), ImVec2(x0 + w, y0 + h), col, s*0.07f);
    }

    // Draws a generic document icon: a page with its corner turned.
    static void fileGlyph(ImDrawList* dl, ImVec2 c, f32 s, ImU32 col) {
        const f32 w = s * 0.74f, h = s;
        const f32 x0 = c.x - w*0.5f, y0 = c.y - h*0.5f, fold = w * 0.34f;
        dl->PathLineTo(ImVec2(x0, y0));
        dl->PathLineTo(ImVec2(x0 + w - fold, y0));
        dl->PathLineTo(ImVec2(x0 + w, y0 + fold));
        dl->PathLineTo(ImVec2(x0 + w, y0 + h));
        dl->PathLineTo(ImVec2(x0, y0 + h));
        dl->PathFillConvex(col);
        dl->AddTriangleFilled(ImVec2(x0 + w - fold, y0), ImVec2(x0 + w, y0 + fold),
                              ImVec2(x0 + w - fold, y0 + fold), IM_COL32(0, 0, 0, 80));
    }

    // Blits one tile of an N-tile sheet, fitted inside an s-by-s box at its own aspect.
    static void blitTile(ImDrawList* dl, u64 tex, ImVec2 centre, f32 s, f32 aspect, int tile, int tiles) {
        const f32 w = aspect >= 1.0f ? s : s * aspect;
        const f32 h = aspect >= 1.0f ? s / aspect : s;
        dl->AddImage(static_cast<ImTextureID>(tex),
                     ImVec2(centre.x - w*0.5f, centre.y - h*0.5f),
                     ImVec2(centre.x + w*0.5f, centre.y + h*0.5f),
                     ImVec2(static_cast<f32>(tile) / tiles, 0.0f),
                     ImVec2(static_cast<f32>(tile + 1) / tiles, 1.0f));
    }

    // Draws one entry's icon, from the sprite sheet where there is one and the drawn glyph otherwise.
    void drawEntryIcon(ImDrawList* dl, ImVec2 centre, f32 s, bool isDir, int tile, bool module) {
        if (isDir) {
            if (folderIconsUiId_) blitTile(dl, folderIconsUiId_, centre, s, folderIconAspect_, module ? 1 : 0, 2);
            else                  folderGlyph(dl, centre, s, IM_COL32(232, 187, 92, 255));
            return;
        }
        if (tile >= kAssetTileBase && assetIconsUiId_) {
            blitTile(dl, assetIconsUiId_, centre, s, assetIconAspect_, tile - kAssetTileBase, kAssetIconTiles);
            return;
        }
        if (tile >= 0 && tile < kAssetTileBase && fileIconsUiId_) {
            blitTile(dl, fileIconsUiId_, centre, s, fileIconAspect_, tile, kFileIconTiles);
            return;
        }
        fileGlyph(dl, centre, s, IM_COL32(150, 154, 162, 255));
    }

    // Returns a path's extension, lower-cased.
    static std::string lowerExt(const std::filesystem::path& p) {
        std::string ext = p.extension().string();
        for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return ext;
    }

    // Every extension the Content Browser will let a user drag into the level -- the single gate
    // BOTH its grid view and its list view check before calling BeginDragDropSource, so a new
    // placeable asset type needs changing here once rather than drifting between the two views.
    // spawnFromAssetDrop is the other half of this contract: it must accept every extension this
    // says yes to, and nothing else.
    static bool isPlaceableAssetExt(const std::string& ext) {
        if (ext == ".ocmesh") return true;
#if AVER_MODULE_PARTICLES
        if (ext == ".ocparticle") return true;
#endif
        return false;
    }

    // True when hay contains needle, ignoring case. An empty needle matches.
    static bool containsNoCase(const std::string& hay, const char* needle) {
        if (!needle || !*needle) return true;
        const usize n = std::strlen(needle);
        if (hay.size() < n) return false;
        for (usize i = 0; i + n <= hay.size(); ++i) {
            usize j = 0;
            while (j < n && std::tolower(static_cast<unsigned char>(hay[i + j])) ==
                            std::tolower(static_cast<unsigned char>(needle[j]))) ++j;
            if (j == n) return true;
        }
        return false;
    }

    // Trims a name to at most `lines` wrapped lines, ending in an ellipsis when it does not fit.
    static std::string fitLabel(const std::string& name, f32 wrap, int lines) {
        const f32 maxH = ImGui::GetTextLineHeight() * lines + 1.0f;
        if (ImGui::CalcTextSize(name.c_str(), nullptr, false, wrap).y <= maxH) return name;
        std::string s = name;
        while (s.size() > 1) {
            s.pop_back();
            const std::string t = s + "...";
            if (ImGui::CalcTextSize(t.c_str(), nullptr, false, wrap).y <= maxH) return t;
        }
        return name;
    }

    // Draws the gallery view: a wrapped, row-clipped grid of icon tiles.
    void drawFolderGallery(const std::vector<const DirEntry*>& shown) {
        const f32 tile   = cbTileSize_ * dpi_;
        const f32 pad    = 8.0f * dpi_;
        const f32 labelH = ImGui::GetTextLineHeight() * 2.0f + 4.0f * dpi_;   // two lines: names wrap
        const f32 cellW  = tile, cellH = tile + labelH;
        int perRow = static_cast<int>((ImGui::GetContentRegionAvail().x + pad) / (cellW + pad));
        if (perRow < 1) perRow = 1;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const int rows = (static_cast<int>(shown.size()) + perRow - 1) / perRow;
        ImGuiListClipper clipper;
        clipper.Begin(rows, cellH + pad);
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                for (int col = 0; col < perRow; ++col) {
                    const int idx = r * perRow + col;
                    if (idx >= static_cast<int>(shown.size())) break;
                    const DirEntry& e = *shown[idx];
                    if (col) ImGui::SameLine(0.0f, pad);
                    ImGui::PushID(idx);
                    const ImVec2 o = ImGui::GetCursorScreenPos();
                    if (ImGui::Selectable("##cell", cbSelectedFile_ == e.full,
                                          ImGuiSelectableFlags_AllowDoubleClick, ImVec2(cellW, cellH))) {
                        cbSelectedFile_ = e.full;
                        if (ImGui::IsMouseDoubleClicked(0) || (e.isDir && !cbDoubleClickEnter_))
                            cbOpenEntry(e.full, e.isDir);
                    }
                    // Only placeable assets start a drag (see isPlaceableAssetExt/spawnFromAssetDrop),
                    // so the viewport drop target never has to reject a payload it received.
                    if (!e.isDir && isPlaceableAssetExt(lowerExt(e.path)) && ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload(kAssetDragDropType, e.full.c_str(), e.full.size() + 1);
                        ImGui::TextUnformatted(e.name.c_str());
                        ImGui::EndDragDropSource();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.name.c_str());
                    cbItemContextMenu(e.full, e.name, e.isDir);
                    drawEntryIcon(dl, ImVec2(o.x + cellW*0.5f, o.y + tile*0.5f), tile*0.52f, e.isDir, e.tile, e.module);
                    const f32 wrap = cellW - 4.0f*dpi_;
                    const std::string label = fitLabel(e.name, wrap, 2);
                    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
                    const f32 tx = ts.x <= wrap ? o.x + (cellW - ts.x)*0.5f : o.x + 2.0f*dpi_;
                    const ImVec4 clip(o.x, o.y + tile, o.x + cellW, o.y + cellH);
                    dl->AddText(nullptr, 0.0f, ImVec2(tx, o.y + tile), ImGui::GetColorU32(ImGuiCol_Text),
                                label.c_str(), nullptr, wrap, &clip);
                    ImGui::PopID();
                }
            }
        }
        clipper.End();
    }

    // Draws the breadcrumb and the folder's filtered entries, as tiles or as a list.
    void drawFolderFiles(std::string dir) {   // by value: a click below reassigns cbSelectedDir_
        drawBreadcrumb(dir);
        ImGui::Separator();

        const DirListing& listing = dirListing(dir);
        std::vector<const DirEntry*> shown;
        shown.reserve(listing.entries.size());
        for (const DirEntry& e : listing.entries)
            if (containsNoCase(e.name, cbFilter_)) shown.push_back(&e);

        if (shown.empty()) {
            ImGui::TextDisabled(listing.entries.empty() ? "(this folder is empty)"
                                                        : "(nothing here matches the search)");
            return;
        }
        if (cbGallery_) { drawFolderGallery(shown); return; }

        const f32 h = ImGui::GetTextLineHeight() * 1.3f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(shown.size()), h);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const DirEntry& e = *shown[i];
                ImGui::PushID(i);
                const ImVec2 o = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(h * 0.78f, h));
                ImGui::SameLine();
                drawEntryIcon(dl, ImVec2(o.x + h*0.39f, o.y + h*0.5f), h*0.82f, e.isDir, e.tile, e.module);
                if (ImGui::Selectable(e.name.c_str(), cbSelectedFile_ == e.full,
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    cbSelectedFile_ = e.full;
                    if (ImGui::IsMouseDoubleClicked(0) || (e.isDir && !cbDoubleClickEnter_))
                        cbOpenEntry(e.full, e.isDir);
                }
                // Only placeable assets start a drag (see isPlaceableAssetExt/spawnFromAssetDrop),
                // so the viewport drop target never has to reject a payload it received.
                if (!e.isDir && isPlaceableAssetExt(lowerExt(e.path)) && ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload(kAssetDragDropType, e.full.c_str(), e.full.size() + 1);
                    ImGui::TextUnformatted(e.name.c_str());
                    ImGui::EndDragDropSource();
                }
                cbItemContextMenu(e.full, e.name, e.isDir);
                ImGui::PopID();
            }
        }
        clipper.End();
    }

    // Draws the Import modal: a source path and the destination folder.
    //
    // A MODAL, and it has to be one. "Browse..." below calls openFileDialog, which opens a native
    // Win32 dialog and blocks this thread until the user dismisses it -- the app stops pumping
    // ImGui entirely, and the OS focus moves to another window. A plain BeginPopup does not survive
    // that reliably: imgui.h:850 says popups "may be closed as any time", and a click over void is
    // expected to close one (imgui.h:2725). BeginPopupModal is the one that "cannot be closed by
    // user" (imgui.h:855), so the popup is still there when the dialog returns and the path it
    // picked has somewhere to land.
    //
    // ProjectBrowser.cpp calls openFileDialog from inside a popup too and is fine -- but that one is
    // BeginPopupModal (ProjectBrowser.cpp:244), so it is not precedent for doing it from a plain
    // popup. The name doubles as the modal's title bar text, hence "Import Asset" rather than the
    // old "cbImport" id; OpenPopup's string was changed to match.
    void drawImportModal() {
        if (!ImGui::BeginPopupModal("Import Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        ImGui::TextUnformatted("Import an asset into the selected folder.");
        ImGui::TextDisabled(".gltf/.glb become .ocmesh; .wav/.mp3/.m4a/.flac become .ocaudio.");
        ImGui::TextDisabled("Anything else is copied as-is.");
        ImGui::SetNextItemWidth(420.0f * dpi_);
        ImGui::InputText("Source file", importPath_, sizeof(importPath_));
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            const std::string start = importPath_[0] != '\0'
                ? std::filesystem::path(importPath_).parent_path().string() : std::string();
#if AVER_HAVE_AUDIO_IMPORT
            const char* kFilterLabel = "Importable assets (*.gltf, *.glb, *.wav, *.mp3, *.m4a, *.flac)";
            const char* kFilterSpec  = "*.gltf;*.glb;*.wav;*.mp3;*.m4a;*.flac";
#else
            const char* kFilterLabel = "Importable assets (*.gltf, *.glb)";
            const char* kFilterSpec  = "*.gltf;*.glb";
#endif
            std::string picked;
            if (openFileDialog("Import asset", kFilterLabel, kFilterSpec,
                               directoryExists(start) ? start : std::string(), picked)) {
                if (picked.size() < sizeof(importPath_)) {
                    std::snprintf(importPath_, sizeof(importPath_), "%s", picked.c_str());
                } else {
                    AVER_WARN("[Import] picked path is {} chars, longer than the {}-char field - not applied",
                              picked.size(), sizeof(importPath_) - 1);
                    cbStatus_ = "Picked path is too long - not applied";
                }
            }
        }
        const std::string dest = cbSelectedDir_.empty() ? project_.contentDir() : cbSelectedDir_;
        const std::string blocked = cbImportBlockedReason(dest);
        if (blocked.empty()) {
            ImGui::Text("Into: %s", dest.c_str());
        } else {
            ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1.0f), "Into: %s - %s", dest.c_str(), blocked.c_str());
        }
        ImGui::BeginDisabled(importPath_[0] == '\0' || !blocked.empty());
        if (ImGui::Button("Import")) {
            importAsset(importPath_, dest);
            importPath_[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Imports one asset into destDir: models and audio are converted, everything else is copied.
    // UI-side: calls cbIsEditable/cbInvalidate/importModel/importAudio, all the content browser's.
    // Its one non-browser caller (--import's deferred handshake in onInit) is guarded instead.
    void importAsset(const std::string& src, const std::string& destDir) {
        std::error_code ec;
        if (!cbIsEditable(destDir)) {
            AVER_WARN("[Import] '{}' is engine content and is read-only", destDir);
            cbStatus_ = "Engine content is read-only";
            return;
        }
        if (!std::filesystem::exists(src, ec)) { AVER_WARN("[Import] source not found: {}", src); return; }
        const std::string name = std::filesystem::path(src).filename().string();
        const std::string dest = destDir + "\\" + name;
        if (std::filesystem::exists(dest, ec)) {
            AVER_WARN("[Import] '{}' already exists in {} - not overwritten; rename the source or remove it first", name, destDir);
            return;
        }
        const std::string ext = std::filesystem::path(src).extension().string();
        std::string lower;
        for (const char c : ext) lower.push_back(c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c);
        if (lower == ".gltf" || lower == ".glb") { importModel(src, destDir); return; }
#if AVER_HAVE_AUDIO_IMPORT
        if (fmt::isImportableAudio(src)) { importAudio(src, destDir); return; }
#endif

        std::filesystem::copy_file(src, dest, ec);
        if (ec) { AVER_WARN("[Import] failed to copy '{}' -> '{}': {}", src, dest, ec.message());
                  cbStatus_ = "Import failed - see the Output Log"; return; }
        AVER_INFO("[Import] imported '{}' into {}", name, destDir);
        cbStatus_ = "Imported " + name;
        cbInvalidate(destDir);
    }

#if AVER_HAVE_AUDIO_IMPORT
    // Decodes .wav / .mp3 / .m4a / .flac into one .ocaudio in destDir. The source is not copied.
    void importAudio(const std::string& src, const std::string& destDir) {
        audio::SoundData data;
        const fmt::AudioImportResult r = fmt::audioImportFile(src, data);
        if (!r.ok) {
            AVER_WARN("[Import] {} could not be decoded: {}",
                      std::filesystem::path(src).filename().string(), r.error);
            cbStatus_ = "Import failed - see the Output Log";
            return;
        }

        const std::string outName = std::filesystem::path(fmt::ocAudioPathFor(src)).filename().string();
        const std::string out = destDir + "\\" + outName;
        std::error_code ec;
        if (std::filesystem::exists(out, ec)) {
            AVER_WARN("[Import] '{}' already exists in {} - not overwritten", outName, destDir);
            cbStatus_ = "Already imported";
            return;
        }

        std::string why;
        if (!fmt::saveOcAudio(out, data, std::filesystem::path(src).filename().string(), &why)) {
            AVER_WARN("[Import] could not write '{}': {}", out, why);
            cbStatus_ = "Import failed - see the Output Log";
            return;
        }

        const f64 seconds = data.sampleRate > 0
                          ? static_cast<f64>(data.samples.size()) /
                            static_cast<f64>(data.channels ? data.channels : 1) /
                            static_cast<f64>(data.sampleRate) : 0.0;
        AVER_INFO("[Import] '{}' -> {} ({} via {}, {} ch, {} Hz, {:.2f} s)",
                  std::filesystem::path(src).filename().string(), outName,
                  data.samples.size(), r.decoder, data.channels, data.sampleRate, seconds);
        cbStatus_ = "Imported " + outName;
        cbInvalidate(destDir);
    }
#endif

    // Converts a glTF/GLB into one .ocmesh per mesh in destDir, and registers them for this session.
    void importModel(const std::string& src, const std::string& destDir) {
        fmt::GltfImportResult res;
        std::string why;
        if (!fmt::importGltf(src, res, {}, &why)) {
            AVER_WARN("[Import] {}", why);
            cbStatus_ = "Import failed - see the Output Log";
            return;
        }
        for (const std::string& u : res.unsupported)
            AVER_WARN("[Import] '{}' contains {} - not imported", std::filesystem::path(src).filename().string(), u);

        std::error_code ec;
        const std::string stem = std::filesystem::path(src).stem().string();
        u32 written = 0;
        for (usize i = 0; i < res.meshes.size(); ++i) {
            fmt::OcMeshData& m = res.meshes[i];
            if (!m.valid()) { AVER_WARN("[Import] mesh {} came out empty and was skipped", i); continue; }

            std::string base = i < res.meshNames.size() && !res.meshNames[i].empty() ? res.meshNames[i] : stem;
            if (res.meshes.size() > 1 && base == stem) base += "_" + std::to_string(i);
            for (char& c : base) if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                                     c == '"' || c == '<' || c == '>' || c == '|') c = '_';

            std::string out = destDir + "\\" + base + ".ocmesh";
            if (std::filesystem::exists(out, ec)) {
                AVER_WARN("[Import] '{}.ocmesh' already exists - not overwritten", base);
                continue;
            }
            if (!fmt::saveOcMesh(out, m, &why)) { AVER_WARN("[Import] {}", why); continue; }
            AVER_INFO("[Import] {} -> {} ({} verts, {} tris)", std::filesystem::path(src).filename().string(),
                      base + ".ocmesh", m.vertexCount(), m.indices.size() / 3);
            ++written;
        }

        // The RIG. This used to drop res.skeletons and res.animations on the floor, so glTF could
        // produce a skeleton and a clip that nothing ever wrote and no project could ever contain --
        // and loadOcSkel/loadOcAnim had no caller in the engine's history.
        u32 rigs = 0, clips = 0;
        for (usize i = 0; i < res.skeletons.size(); ++i) {
            std::string base = i < res.skeletonNames.size() && !res.skeletonNames[i].empty()
                             ? res.skeletonNames[i] : stem;
            if (res.skeletons.size() > 1) base += "_" + std::to_string(i);
            sanitiseAssetName(base);
            const std::string out = destDir + "\\" + base + ".ocskel";
            if (std::filesystem::exists(out, ec)) {
                AVER_WARN("[Import] '{}.ocskel' already exists - not overwritten", base);
            } else if (!fmt::saveOcSkel(out, res.skeletons[i], &why)) {
                AVER_WARN("[Import] {}", why);
            } else {
                AVER_INFO("[Import] {} -> {} ({} bone(s))", std::filesystem::path(src).filename().string(),
                          base + ".ocskel", res.skeletons[i].bones.size());
                ++rigs;
            }
        }
        for (usize i = 0; i < res.animations.size(); ++i) {
            std::string base = i < res.animationNames.size() && !res.animationNames[i].empty()
                             ? res.animationNames[i] : (stem + "_clip" + std::to_string(i));
            sanitiseAssetName(base);
            const std::string out = destDir + "\\" + base + ".ocanim";
            if (std::filesystem::exists(out, ec)) {
                AVER_WARN("[Import] '{}.ocanim' already exists - not overwritten", base);
            } else if (!fmt::saveOcAnim(out, res.animations[i], &why)) {
                AVER_WARN("[Import] {}", why);
            } else {
                AVER_INFO("[Import] {} -> {} ({:.2f}s, {} track(s))",
                          std::filesystem::path(src).filename().string(), base + ".ocanim",
                          res.animations[i].duration, res.animations[i].tracks.size());
                ++clips;
            }
        }

        if (written == 0 && rigs == 0 && clips == 0) {
            cbStatus_ = "Import produced nothing - see the Output Log";
            return;
        }
        cbStatus_ = "Imported " + std::to_string(written) + " mesh(es), " + std::to_string(rigs) +
                    " skeleton(s) and " + std::to_string(clips) + " clip(s) from " +
                    std::filesystem::path(src).filename().string();
        cbInvalidate(destDir);
        wantMeshReload_ = true;
    }

    // Replaces the characters Windows refuses in a file name. Asset names come from a glTF, so they
    // are whatever the authoring tool allowed.
    static void sanitiseAssetName(std::string& s) {
        for (char& c : s)
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        if (s.empty()) s = "unnamed";
    }

    // Writes an edited material back to the .cs under Content\Materials that declares it, found by
    // trying each in turn. Returns the file written, or "" with err set.
    //
    // THE GUARD IS ABOVE THE SIGNATURE, not inside the body, and it was inside. `pbr::MaterialDesc`
    // is in the parameter list, so with PBR off the function did not compile at all -- the #if was
    // protecting the body from a type the signature had already required.
#if AVER_MODULE_PBR
    std::string saveMaterialSource(const std::string& name, const pbr::MaterialDesc& d, std::string& err) {
        namespace fs = std::filesystem;
        const std::string content = project_.contentDir();
        if (content.empty()) { err = "no project"; return {}; }
        const fs::path dir = fs::path(content) / "Materials";
        std::error_code ec;
        if (!fs::exists(dir, ec)) { err = "no Content\\Materials directory"; return {}; }

        std::string firstError;
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec) || it->path().extension() != ".cs") continue;
            const std::string path = it->path().string();

            std::ifstream in(path, std::ios::binary);
            if (!in) continue;
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();

            std::string out, why;
            if (!fmt::rewriteMaterialScript(text, name, d, nullptr, out, &why)) {
                if (text.find("[AverMaterial(\"" + name + "\")]") != std::string::npos && firstError.empty())
                    firstError = why;
                continue;
            }
            if (out == text) return path;

            std::ofstream os(path, std::ios::binary | std::ios::trunc);
            if (!os) { err = "could not open " + path + " for writing"; return {}; }
            os.write(out.data(), static_cast<std::streamsize>(out.size()));
            if (!os) { err = "write failed"; return {}; }
            return path;
        }
        err = firstError.empty() ? ("no .cs under Content\\Materials declares '" + name + "'") : firstError;
        return {};
    }
#endif  // AVER_MODULE_PBR

    // Draws the material half of the Details panel, editing the shared MaterialDesc where there is
    // one and the actor's own values otherwise.
    void materialPanel(MeshObj& o) {
#if AVER_MODULE_PBR
        pbr::MaterialDesc* d = pbr::MaterialLibrary::get().mutableDesc(o.material);
        if (!d) { ImGui::TextDisabled("No material (drawing with the fallback)"); return; }
        bool changed = false;
        changed |= ImGui::SliderFloat("Metallic", &d->metallicFactor, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("Roughness", &d->roughnessFactor, 0.045f, 1.0f);
        changed |= ImGui::SliderFloat("Normal Scale", &d->normalScale, 0.0f, 4.0f);
        changed |= ImGui::SliderFloat("Occlusion", &d->occlusionStrength, 0.0f, 1.0f);
        // 0..0.2 covers water (~0.02) through gemstone (~0.17).
        changed |= ImGui::SliderFloat("Reflectance", &d->reflectance, 0.0f, 0.2f, "%.3f");
        changed |= ImGui::SliderFloat("Grazing (f90)", &d->f90, 0.0f, 1.0f);
        changed |= ImGui::DragFloat3("Emissive", d->emissiveFactor, 0.01f, 0.0f, 32.0f);

        ImGui::Separator();
        int uvMode = static_cast<int>(d->uvMode);
        if (ImGui::Combo("UV Mapping", &uvMode, "Mesh UVs\0World Aligned\0")) {
            d->uvMode = static_cast<pbr::UvMode>(uvMode);
            changed = true;
        }
        if (d->uvMode == pbr::UvMode::WorldAligned) {
            changed |= ImGui::SliderFloat("Tile Size (cm)", &d->uvTiling, 5.0f, 2000.0f, "%.0f",
                                          ImGuiSliderFlags_Logarithmic);
        }

        ImGui::Separator();
        for (u32 s = 0; s < pbr::kTextureSlotCount; ++s) {
            char buf[260];
            const std::string& p = d->textures[s].path;
            std::snprintf(buf, sizeof(buf), "%s", p.c_str());
            if (ImGui::InputText(pbr::MaterialLibrary::textureSlotName(static_cast<pbr::TextureSlot>(s)),
                                 buf, sizeof(buf))) {
                d->textures[s].path = buf;
                changed = true;
            }
        }
        if (changed) pbr::MaterialLibrary::get().touch(o.material);

        ImGui::Separator();
        ImGui::BeginDisabled(!project_.valid() || d->name.empty());
        if (ImGui::Button("Save to C#")) {
            std::string err;
            const std::string file = saveMaterialSource(d->name, *d, err);
            if (file.empty()) {
                matSaveStatus_ = "Could not save: " + err;
                AVER_ERROR("[Material] save failed for '{}': {}", d->name, err);
            } else {
                matSaveStatus_ = "Saved to " + std::filesystem::path(file).filename().string() +
                                 " - Compile C# to regenerate the .ocmat";
                AVER_INFO("[Material] '{}' written back to {}", d->name, file);
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(project_.valid()
                ? "Rewrites this material's Configure in Content\\Materials.\n"
                  "The .ocmat under Binaries is regenerated by Compile C#."
                : "Open a project first - a material belongs to one.");
        if (!matSaveStatus_.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", matSaveStatus_.c_str()); }
#else
        ImGui::SliderFloat("Metallic", &o.metallic, 0.0f, 1.0f);
        ImGui::SliderFloat("Roughness", &o.roughness, 0.02f, 1.0f);
#endif
    }

    // Draws the World Outliner and the Details panel.
    void buildPanels(Engine& e) {
        (void)e;
        ImGui::Begin("World Outliner");
        if (!hideEditorScene_)
            for (int i=0;i<(int)objects_.size();++i)
                if (ImGui::Selectable((std::string("  ")+objects_[i].name).c_str(), sel_==i)) { sel_=i; selEntity_=kInvalidId; }
#if AVER_MODULE_SCENE
        {
            scene::World& w = scene::World::instance();
            const u32 n = w.count();
            int listed = 0;
            for (u32 i = 0; i < n; ++i) {
                const scene::Entity ent = w.at(i);
                if (!w.valid(ent) || w.destroyPending(ent)) continue;
                // Chunk-streamed entities are excluded on purpose: potentially hundreds of them come
                // and go as the camera moves, and this list is for what a designer placed. Their
                // live count is in the Chunk Streaming stats window instead (buildChunkStreamingPanel).
                if (anyChunkWorldOwns(ent)) continue;
                // The graph-driven drone is excluded for the identical reason: transient, not
                // authored, tracked separately (see the [Drone] AVER_INFO lines / the Details panel
                // if selected directly some other way -- there isn't one; it just isn't listed here).
                if (ent == droneEntity_) continue;
                const auto* mr = w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
                const std::string nm = w.name(ent);
                if (!mr && nm.empty()) continue;
                if (listed++ == 0 && !hideEditorScene_) ImGui::Separator();
                const auto lit = entityLabels_.find(static_cast<u32>(ent));
                const std::string shown = lit != entityLabels_.end() ? lit->second
                                        : (nm.empty() ? ("Entity " + std::to_string((u32)ent)) : nm);
                const std::string label = "  " + shown + "##e" + std::to_string((u32)ent);
                if (ImGui::Selectable(label.c_str(), sel_==kSelScene && selEntity_==ent)) {
                    sel_ = kSelScene; selEntity_ = ent;
                }
            }
        }
#endif
        ImGui::Separator();
        // Unguarded: the sun/sky/post-process pseudo-entries exist whether or not there is a scene.
        if (ImGui::Selectable("  Directional Light (Sun)", sel_==-2)) { sel_=-2; selEntity_=kInvalidId; }
        if (ImGui::Selectable("  Sky + Atmosphere", sel_==-3))        { sel_=-3; selEntity_=kInvalidId; }
        if (ImGui::Selectable("  Post Process", sel_==-4))            { sel_=-4; selEntity_=kInvalidId; }
        ImGui::End();

        ImGui::Begin("Details");
        if (sel_>=0 && sel_<(int)objects_.size()){
            MeshObj& o=objects_[sel_]; ImGui::TextUnformatted(o.name.c_str()); ImGui::Separator();
            if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::DragFloat3("Location (cm)", &o.pos.x, 1.0f);
                ImGui::DragFloat3("Rotation", &o.rotDeg.x, 1.0f);
                ImGui::DragFloat3("Scale", &o.scale.x, 0.01f, 0.02f, 100.f);
            }
            if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::ColorEdit3("Base Color", o.color);
                materialPanel(o);
            }
            ImGui::Checkbox("Visible", &o.visible);
#if AVER_MODULE_SCENE
        } else if (sel_==kSelScene && scene::World::instance().valid(selEntity_)) {
            scene::World& w = scene::World::instance();
            const std::string nm = w.name(selEntity_);
            const auto lit = entityLabels_.find(static_cast<u32>(selEntity_));
            ImGui::TextUnformatted(lit != entityLabels_.end() ? lit->second.c_str()
                                                             : (nm.empty() ? "(unnamed entity)" : nm.c_str()));
            ImGui::SameLine(); ImGui::TextDisabled("#%u  %s", (u32)selEntity_, nm.c_str());
            ImGui::Separator();
            if (const auto* loc = w.component<scene::CLocal>(selEntity_, scene::kComponentLocal)) {
                if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
                    Transform xf = loc->xf;
                    Vec3 euler = eulerDegFromQuat(xf.rotation);
                    bool moved = ImGui::DragFloat3("Location (cm)", &xf.position.x, 1.0f);
                    if (ImGui::IsItemActivated()) beginTransformEdit();
                    bool done = ImGui::IsItemDeactivatedAfterEdit();
                    if (ImGui::DragFloat3("Rotation", &euler.x, 1.0f)) {
                        xf.rotation = quatFromEulerDeg(euler); moved = true;
                    }
                    if (ImGui::IsItemActivated()) beginTransformEdit();
                    done = done || ImGui::IsItemDeactivatedAfterEdit();
                    moved |= ImGui::DragFloat3("Scale", &xf.scale.x, 0.5f, 0.01f, 100000.0f);
                    if (ImGui::IsItemActivated()) beginTransformEdit();
                    done = done || ImGui::IsItemDeactivatedAfterEdit();
                    if (moved) w.setLocalTransform(selEntity_, xf);
                    if (done) endTransformEdit();
                }
            }
            if (auto* mr = w.component<scene::CMeshRenderer>(selEntity_, scene::kComponentMeshRenderer)) {
                if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
                    bool vis = (mr->flags & scene::kMeshRendererVisible) != 0;
                    if (ImGui::Checkbox("Visible", &vis)) {
                        if (vis) mr->flags |=  scene::kMeshRendererVisible;
                        else     mr->flags &= ~scene::kMeshRendererVisible;
                    }
                    ImGui::TextDisabled("mesh id 0x%llx", (unsigned long long)mr->mesh);
                }
            }
#if AVER_MODULE_PARTICLES
            // The authoring surface DECIDED components need: visible and editable the same way
            // CMeshRenderer just above is. No picker widget beyond drag-drop exists for CMeshRenderer::
            // mesh either (assignment happens by placing a NEW entity via spawnFromAssetDrop), so an
            // emitter's own effect id follows that same, already-established shape rather than
            // inventing a combo-box asset browser for this one field.
            if (auto* pe = w.component<scene::CParticleEmitter>(selEntity_, scene::kComponentParticleEmitter)) {
                if (ImGui::CollapsingHeader("Particle Emitter", ImGuiTreeNodeFlags_DefaultOpen)) {
                    bool stopped = (pe->flags & scene::kParticleEmitterStopped) != 0;
                    if (ImGui::Checkbox("Stopped", &stopped)) {
                        if (stopped) pe->flags |=  scene::kParticleEmitterStopped;
                        else         pe->flags &= ~scene::kParticleEmitterStopped;
                    }
                    ImGui::TextDisabled("effect id 0x%llx", (unsigned long long)pe->effect);
                    // Drop a .ocparticle from the Content Browser directly onto this row to point this
                    // emitter at it -- the SAME id space loadProjectParticleEffects() populates (this
                    // very panel's own fnv1a64(relative path)), so a freshly authored effect resolves
                    // the moment it lands here, mirroring how dropping a .ocmesh on the viewport
                    // (spawnFromAssetDrop) places one.
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetDragDropType)) {
                            const std::string dropped(
                                static_cast<const char*>(payload->Data),
                                payload->DataSize > 0 ? static_cast<usize>(payload->DataSize - 1) : usize(0));
                            if (lowerExt(std::filesystem::path(dropped)) == ".ocparticle") {
                                const std::string content = project_.contentDir();
                                std::error_code ec;
                                std::string rel = content.empty() ? std::string()
                                    : std::filesystem::relative(dropped, content, ec).string();
                                if (!content.empty() && !ec && !rel.empty()) {
                                    for (char& c : rel) if (c == '\\') c = '/';
                                    pe->effect = fnv1a64(std::string_view(rel));
                                    cbStatus_ = "Assigned " + std::filesystem::path(dropped).filename().string();
                                    AVER_INFO("[Particles] entity {} effect set to 0x{:016X} ('{}')",
                                              selEntity_, pe->effect, rel);
                                } else {
                                    cbStatus_ = "Could not resolve the dropped effect to a project-relative path";
                                }
                            } else {
                                cbStatus_ = "Only a .ocparticle asset can be assigned to an emitter";
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    ImGui::TextDisabled("age %.2fs   seed 0x%08x", pe->age, pe->seed);
                }
            }
#endif
#endif
        } else if (sel_==-2){
            ImGui::TextUnformatted("Directional Light (Sun)"); ImGui::Separator();
            {
                f32 elev = 0.0f, azim = 0.0f;
                sky_.sunAngles(elev, azim);
                bool moved = ImGui::SliderFloat("Elevation", &elev, -20.0f, 90.0f, "%.1f deg");
                moved |= ImGui::SliderFloat("Azimuth", &azim, -180.0f, 180.0f, "%.1f deg");
                // Written back only on a real move: the angle round trip is lossy.
                if (moved) sky_.setSunAngles(elev, azim);
            }
            ImGui::SliderFloat("Intensity", &sky_.sunIntensity, 0.0f, 8.0f, "%.2f");
            bool useTemp = sky_.sunTemperatureK > 0.0f;
            if (ImGui::Checkbox("Use Colour Temperature", &useTemp))
                sky_.sunTemperatureK = useTemp ? 5500.0f : 0.0f;
            if (useTemp) {
                ImGui::SliderFloat("Temperature", &sky_.sunTemperatureK, 1500.0f, 12000.0f, "%.0f K");
            } else {
                ImGui::ColorEdit3("Colour", sunColor_);
            }
            ImGui::SliderFloat("Angular Size", &sky_.sunAngularDiameterDeg, 0.05f, 8.0f, "%.2f deg");
        } else if (sel_==-3){
            ImGui::TextUnformatted("Sky + Atmosphere"); ImGui::Separator();
            {
                int model = sky_.model == rhi::SkyModel::Physical ? 1 : 0;
                const char* names[] = {"Authored (two-colour dome)", "Physical (Rayleigh + Mie + ozone)"};
                if (ImGui::Combo("Sky Model", &model, names, 2))
                    sky_.model = model ? rhi::SkyModel::Physical : rhi::SkyModel::Authored;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Physical derives the dome below from the sun's elevation.\n"
                                      "Sunset stops being a colour somebody picked and becomes geometry.");
            }
            const bool physicalSky = sky_.model == rhi::SkyModel::Physical;

            ImGui::BeginDisabled(physicalSky);
            ImGui::ColorEdit3("Zenith", skyZenith_); ImGui::ColorEdit3("Horizon", skyHorizon_);
            ImGui::SliderFloat("Atmosphere Height", &sky_.atmosphereHeight, 0.05f, 4.0f, "%.2f");
            ImGui::EndDisabled();
            if (physicalSky) {
                ImGui::TextDisabled("^ derived from the sun's elevation while the model is Physical");
                if (ImGui::TreeNode("Air")) {
                    ImGui::SliderFloat("Mie Scatter", &sky_.air.mieScatter, 0.0f, 4e-2f, "%.5f /km",
                                       ImGuiSliderFlags_Logarithmic);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Aerosol. Raise it for haze, dust or a coastal day; the\n"
                                          "sun's halo and the pale band at the horizon both grow.");
                    ImGui::SliderFloat("Mie Extinction", &sky_.air.mieExtinction, 0.0f, 4e-2f, "%.5f /km",
                                       ImGuiSliderFlags_Logarithmic);
                    ImGui::SliderFloat("Mie Anisotropy", &sky_.air.miePhaseG, 0.0f, 0.95f, "%.2f");
                    ImGui::SliderFloat("Rayleigh Height", &sky_.air.rayleighScaleKm, 1.0f, 20.0f, "%.1f km");
                    ImGui::SliderFloat("Mie Height", &sky_.air.mieScaleKm, 0.2f, 8.0f, "%.2f km");
                    ImGui::SliderFloat("Multi-Scatter", &sky_.air.multiScatterGain, 0.0f, 4.0f, "%.2f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Stands in for every bounce after the first. 1.70 is\n"
                                          "calibrated: it puts diffuse light at 15-30%% of direct\n"
                                          "and the zenith's blue/red between 2.5 and 5.");
                    ImGui::SliderFloat("Planet Radius", &sky_.air.planetRadiusKm, 100.0f, 20000.0f, "%.0f km");
                    ImGui::SliderFloat("Air Depth", &sky_.air.atmosphereHeightKm, 5.0f, 200.0f, "%.0f km");
                    ImGui::SliderInt("Sky Steps", &sky_.air.viewSteps, 4, 96);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Samples along a sky ray. 32 is within 3%% of a converged\n"
                                          "march; below about 16 the horizon starts to band.");
                    ImGui::SliderInt("Aerial Steps", &sky_.air.aerialSteps, 1, 16);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Samples along the air between the camera and a surface.\n"
                                          "This one runs per shaded pixel, so it is the expensive one.");
                    ImGui::TreePop();
                }
            }
            ImGui::ColorEdit3("Ground", sky_.groundAlbedo);
            ImGui::SliderFloat("Ground Blend", &sky_.groundBlend, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Sky Light", &sunAmbient_, 0.0f, 2.0f, "%.2f");

            ImGui::Separator();
            ImGui::TextUnformatted("Height Fog");
            ImGui::ColorEdit3("Fog Tint", fogColor_);
            ImGui::SliderFloat("Fog Density", &fogDensity_, 1e-7f, 1e-3f, "%.2e",
                               ImGuiSliderFlags_Logarithmic);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Extinction per cm. Visibility = 3.912 / density.\n"
                                  "2e-6 clear (20 km)   4e-6 light haze (10 km)\n"
                                  "8e-6 haze (5 km)     8e-5 fog (500 m)");
            ImGui::SliderFloat("Height Falloff", &sky_.fogFalloff, 0.0f, 0.02f, "%.5f",
                               ImGuiSliderFlags_Logarithmic);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = uniform distance fog (the old behaviour)");
            ImGui::DragFloat("Fog Height", &sky_.fogHeight, 1.0f);
            ImGui::DragFloat("Fog Start", &sky_.fogStart, 1.0f, 0.0f, 1e6f);
            ImGui::SliderFloat("Max Opacity", &sky_.fogMaxOpacity, 0.0f, 1.0f, "%.2f");
#if AVER_MODULE_SCENE
            if (chunkWorld_) {
                const world::StreamSettings& mst = chunkWorld_->settings().stream;
                const f32 boundaryCm = static_cast<f32>(mst.loadRadius) * static_cast<f32>(mst.chunkSizeCm);
                ImGui::Checkbox("Match Fog To Streaming Radius", &matchFogToStreamRadius_);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Recomputes Fog Density every frame -- overriding the slider\n"
                                      "above -- so opacity AT the load boundary (%.0fcm here) equals\n"
                                      "the target below. NOT a free win: hiding a boundary this close\n"
                                      "typically needs an order of magnitude more density than a\n"
                                      "level's authored default, i.e. a visibly foggier world.", boundaryCm);
                if (matchFogToStreamRadius_) {
                    ImGui::SliderFloat("Target Opacity At Boundary", &fogMatchTargetOpacity_, 0.5f, 0.99f, "%.2f");
                    const f32 matched = fogDensityForOpacityAt(boundaryCm, fogMatchTargetOpacity_);
                    ImGui::TextDisabled("boundary %.0fcm -> density %.2e", boundaryCm, matched);
                }
            }
#endif

            ImGui::Separator();
            ImGui::TextUnformatted("Volumetric Clouds");
            ImGui::Checkbox("Clouds", &sky_.cloudsEnabled);
            if (sky_.cloudsEnabled) {
                ImGui::SliderFloat("Coverage", &sky_.cloudCoverage, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Density", &sky_.cloudDensity, 0.0f, 4.0f, "%.2f");
                ImGui::DragFloat("Layer Bottom", &sky_.cloudBottom, 100.0f);
                ImGui::DragFloat("Layer Top", &sky_.cloudTop, 100.0f);
                f32 featureSize = sky_.cloudScale > 1e-9f ? 1.0f / sky_.cloudScale : 50000.0f;
                if (ImGui::DragFloat("Feature Size", &featureSize, 100.0f, 100.0f, 5e6f))
                    sky_.cloudScale = 1.0f / std::fmax(featureSize, 1.0f);
                ImGui::DragFloat2("Wind", sky_.cloudWind, 5.0f);
            }
        } else if (sel_==-4){
            ImGui::TextUnformatted("Post Process"); ImGui::Separator();
            ImGui::BeginDisabled(post_.autoExposure);
            ImGui::SliderFloat("Exposure", &post_.exposure, 0.05f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            ImGui::EndDisabled();
            ImGui::Checkbox("Auto Exposure", &post_.autoExposure);
            if (post_.autoExposure) {
                ImGui::SliderFloat("Middle Grey", &post_.exposureKey, 0.02f, 0.6f, "%.3f");
                ImGui::SliderFloat("Adapt Speed", &post_.exposureSpeed, 0.1f, 20.0f, "%.1f/s");
                ImGui::SliderFloat("Exposure Min", &post_.exposureMin, 0.01f, 1.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
                ImGui::SliderFloat("Exposure Max", &post_.exposureMax, 1.0f, 64.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
            }
            ImGui::Separator();
            ImGui::SliderFloat("Bloom", &post_.bloomIntensity, 0.0f, 1.0f, "%.3f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Zero skips the whole bloom pyramid, not just its weight");
            if (post_.bloomIntensity > 0.0f) {
                ImGui::SliderFloat("Threshold", &post_.bloomThreshold, 0.0f, 8.0f, "%.2f");
                ImGui::SliderFloat("Knee", &post_.bloomKnee, 0.0f, 2.0f, "%.2f");
            }
        } else ImGui::TextDisabled("Select an actor in the World Outliner");
        ImGui::End();

    }

    // Draws the bottom drawer, sliding the Content Browser or the Output Log up over the viewport.
    void drawDrawer(Engine& e) {
        const f32 dt = std::fmin(e.time().dt, 0.05f);
        const f32 target = drawer_ == Drawer::None ? 0.0f : 1.0f;
        drawerAnim_ += (target - drawerAnim_) * (1.0f - std::exp(-drawerRate_ * dt));
        if (drawer_ != Drawer::None) drawerShown_ = drawer_;
        if (drawer_ == Drawer::None && drawerAnim_ < 0.004f) { drawerAnim_ = 0.0f; return; }

        const ImGuiViewport* mv = ImGui::GetMainViewport();
        const ImVec2 wpos = mv->WorkPos, wsize = mv->WorkSize;
        const f32 statusH = 26.0f * dpi_;
        const f32 fullH = (wsize.y - statusH) * drawerFrac_;
        const f32 h = fullH * drawerAnim_;

        ImGui::SetNextWindowPos(ImVec2(wpos.x, wpos.y + wsize.y - statusH - h));
        ImGui::SetNextWindowSize(ImVec2(wsize.x, h));
        if (drawerRaise_) { ImGui::SetNextWindowFocus(); drawerRaise_ = false; }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0.0f, 0.0f));
        ImGui::Begin("##drawer", nullptr, kDrawerFlags);

        // The top edge is a resize grip, claimed before anything else is submitted.
        const f32 gripH = 5.0f * dpi_;
        ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
        ImGui::InvisibleButton("##drawergrip", ImVec2(std::fmax(wsize.x, 1.0f), gripH));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive() && wsize.y > statusH + 1.0f) {
            drawerFrac_ -= ImGui::GetIO().MouseDelta.y / (wsize.y - statusH);
            drawerFrac_ = std::fmin(0.88f, std::fmax(0.14f, drawerFrac_));
        }
        ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x, gripH + ImGui::GetStyle().WindowPadding.y));
        ImGui::GetWindowDrawList()->AddLine(ImVec2(wpos.x, ImGui::GetWindowPos().y),
                                            ImVec2(wpos.x + wsize.x, ImGui::GetWindowPos().y),
                                            ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);

        if (fontMedium_) ImGui::PushFont(fontMedium_, 0.0f);
        ImGui::TextUnformatted(drawerShown_ == Drawer::Log ? "Output Log" : "Content Browser");
        if (fontMedium_) ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled(drawerShown_ == Drawer::Content ? "(Ctrl+Space or Esc to dismiss)" : "(Esc to dismiss)");
        ImGui::SameLine(std::fmax(ImGui::GetCursorPosX(), wsize.x - 34.0f * dpi_));
        if (ImGui::Button("X")) drawer_ = Drawer::None;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close the drawer");
        ImGui::Separator();

        if (ImGui::GetContentRegionAvail().y > ImGui::GetFrameHeight()) {
            if (drawerShown_ == Drawer::Log) drawOutputLog();
            else                             drawContentBrowser();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    // Opens a drawer, or closes it if it is already the one showing.
    void toggleDrawer(Drawer d) {
        drawer_ = (drawer_ == d) ? Drawer::None : d;
        if (drawer_ != Drawer::None) drawerRaise_ = true;
    }

    // Reads every editor preference into the members that back the widgets.
    void loadEditorPreferences() {
        using namespace editor;
        cbGallery_          = prefBool ("contentBrowser.gallery",        cbGallery_);
        cbTileSize_         = prefFloat("contentBrowser.tileSize",       cbTileSize_);
        cbDoubleClickEnter_ = prefBool ("contentBrowser.dblClickEnter",  cbDoubleClickEnter_);
        drawerFrac_         = prefFloat("drawers.heightFraction",        drawerFrac_);
        drawerRate_         = prefFloat("drawers.slideRate",             drawerRate_);
        logAutoScroll_      = prefBool ("outputLog.autoScroll",          logAutoScroll_);
        logLevelFilter_     = prefInt  ("outputLog.levelFilter",         logLevelFilter_);
        showGrid_           = prefBool ("viewport.showGrid",             showGrid_);
        wireframe_          = prefBool ("viewport.wireframe",            wireframe_);
        flySpeed_           = prefFloat("viewport.flySpeed",             flySpeed_);
        lookSpeed_          = prefFloat("viewport.lookSensitivity",      lookSpeed_);

        prefIdeName_ = prefString("contentBrowser.ide", "");

        if (prefsDevice_ && prefsDevice_->vsyncCanDisable())
            prefsDevice_->setVSync(prefBool("display.vsync", prefsDevice_->vsync()));
        if (prefsDevice_ && renderScaleOverride_ == 1.0f)   // --render-scale on the command line wins
            prefsDevice_->setRenderScale(prefFloat("display.renderScale", prefsDevice_->renderScale()));

        keybinds_.loadFromPrefs();
    }

    // Resolves the stored IDE name to an index, once the async scan has produced the list.
    void resolvePreferredIdeFromPrefs() {
        if (prefIdeName_.empty() || !editor::ideDetectionFinished()) return;
        const std::vector<editor::IdeInfo>& ides = editor::detectedIdes();
        for (usize i = 0; i < ides.size(); ++i)
            if (ides[i].name == prefIdeName_) { cbIdeChoice_ = static_cast<int>(i); break; }
        prefIdeName_.clear();
    }

    // Writes every editor preference back and flushes. Each setter compares before it stores.
    void saveEditorPreferences() {
        using namespace editor;
        setPrefBool ("contentBrowser.gallery",       cbGallery_);
        setPrefFloat("contentBrowser.tileSize",      cbTileSize_);
        setPrefBool ("contentBrowser.dblClickEnter", cbDoubleClickEnter_);
        setPrefFloat("drawers.heightFraction",       drawerFrac_);
        setPrefFloat("drawers.slideRate",            drawerRate_);
        setPrefBool ("outputLog.autoScroll",         logAutoScroll_);
        setPrefInt  ("outputLog.levelFilter",        logLevelFilter_);
        setPrefBool ("viewport.showGrid",            showGrid_);
        setPrefBool ("viewport.wireframe",           wireframe_);
        setPrefFloat("viewport.flySpeed",            flySpeed_);
        setPrefFloat("viewport.lookSensitivity",     lookSpeed_);

        const std::vector<editor::IdeInfo>& ides = editor::detectedIdes();
        if (cbIdeChoice_ >= 0 && cbIdeChoice_ < static_cast<int>(ides.size()))
            setPrefString("contentBrowser.ide", ides[static_cast<usize>(cbIdeChoice_)].name);
        else
            setPrefString("contentBrowser.ide", "");   // Automatic

        if (prefsDevice_ && prefsDevice_->vsyncCanDisable())
            setPrefBool("display.vsync", prefsDevice_->vsync());
        if (prefsDevice_)
            setPrefFloat("display.renderScale", prefsDevice_->renderScale());

        keybinds_.saveToPrefs();

        flushEditorPrefs();
    }

    // Draws the Editor Preferences window: how this machine's editor behaves.
    void buildEditorPrefs() {
        resolvePreferredIdeFromPrefs();
        if (!showEditorPrefs_) return;
        const ImGuiViewport* mv = ImGui::GetMainViewport();
        // 460 -> 640: six sections (the Keybinds table added a sixth) no longer fit the old height
        // on a typical monitor without immediately scrolling; still just a FirstUseEver default, so
        // anyone who has already resized this window keeps their own size.
        ImGui::SetNextWindowSize(ImVec2(560.0f*dpi_, 640.0f*dpi_), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(mv->GetCenter().x, mv->GetCenter().y), ImGuiCond_FirstUseEver, ImVec2(0.5f,0.5f));
        if (!ImGui::Begin("Editor Preferences", &showEditorPrefs_, ImGuiWindowFlags_NoDocking)) { ImGui::End(); return; }

        if (ImGui::CollapsingHeader("Content Browser", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Open in gallery (tiles) view", &cbGallery_);
            ImGui::SliderFloat("Tile size", &cbTileSize_, 56.0f, 168.0f, "%.0f dp");
            ImGui::Checkbox("Double-click a folder to enter it", &cbDoubleClickEnter_);
            ImGui::TextDisabled("Single-click always selects; the tree navigates either way.");
            const std::vector<editor::IdeInfo>& ides = editor::detectedIdes();
            std::string label = cbIdeChoice_ < 0 || cbIdeChoice_ >= static_cast<int>(ides.size())
                              ? "Automatic (" + editor::preferredIde().name + ")"
                              : ides[static_cast<usize>(cbIdeChoice_)].name;
            if (ImGui::BeginCombo("Open source in", label.c_str())) {
                if (ImGui::Selectable("Automatic", cbIdeChoice_ < 0)) cbIdeChoice_ = -1;
                for (usize i = 0; i < ides.size(); ++i)
                    if (ImGui::Selectable(ides[i].name.c_str(), cbIdeChoice_ == static_cast<int>(i)))
                        cbIdeChoice_ = static_cast<int>(i);
                ImGui::EndCombo();
            }
            if (!editor::ideDetectionFinished())
                ImGui::TextDisabled("Still scanning for installed IDEs...");
        }
        if (ImGui::CollapsingHeader("Drawers", ImGuiTreeNodeFlags_DefaultOpen)) {
            f32 pct = drawerFrac_ * 100.0f;
            if (ImGui::SliderFloat("Height", &pct, 14.0f, 88.0f, "%.0f%% of the window"))
                drawerFrac_ = pct / 100.0f;
            ImGui::SliderFloat("Slide speed", &drawerRate_, 4.0f, 40.0f, "%.0f");
            ImGui::TextDisabled("Ctrl+Space opens the Content Browser; Esc dismisses a drawer.");
        }
        if (ImGui::CollapsingHeader("Output Log", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Auto-scroll to the newest line", &logAutoScroll_);
            ImGui::Combo("Level filter", &logLevelFilter_, "All\0Info+\0Warn+\0");
        }
        if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
            const bool canDisable = prefsDevice_ && prefsDevice_->vsyncCanDisable();
            bool vs = prefsDevice_ ? prefsDevice_->vsync() : true;
            ImGui::BeginDisabled(!canDisable);
            if (ImGui::Checkbox("V-Sync", &vs) && prefsDevice_) prefsDevice_->setVSync(vs);
            ImGui::EndDisabled();
            if (!canDisable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("This display path cannot tear, so vsync cannot be turned off.\n"
                                  "Needs DXGI tearing support (DXGI 1.5+).");
            ImGui::SameLine();
            ImGui::TextDisabled(vs ? "(capped to the refresh rate)" : "(uncapped, may tear)");

#if AVER_MODULE_SR
            // AverSR quality (docs/AVERSR.md "Quality levels"): a named shortcut into the SAME
            // render-scale knob the slider just below edits directly, nothing more -- selecting a
            // level here is exactly equivalent to dragging the slider to its table value by hand.
            // Because of that, this combo shows the last level CHOSEN through it or --aversr, not a
            // live read of whether the current scale still matches one: dragging the slider
            // afterwards moves the scale without moving this combo back to "Custom". Cheap and
            // honest about what it is, not a full two-way-bound settings pair.
            static const char* kAverSrNames[] = {"Off", "Quality", "Balanced", "Performance"};
            int aversrIdx = static_cast<int>(averSrQuality_);
            if (ImGui::Combo("AverSR", &aversrIdx, kAverSrNames, 4) && prefsDevice_)
                applyAverSrQuality(prefsDevice_, static_cast<aver::sr::Quality>(aversrIdx));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Aver Super Resolution: renders the scene smaller and resamples it\n"
                                  "back up. Off is bit-identical to no AverSR at all. See docs/AVERSR.md\n"
                                  "-- on this build, this sets render scale for real; the resample pass\n"
                                  "itself is not yet wired into what actually reaches the screen.");
#endif
            // Render scale: the 3D scene's own resolution as a fraction of the window's. 1.0 (the
            // right edge) is the pre-existing behaviour -- the scene renders 1:1 with the window --
            // and everything below it trades scene sharpness for every pixel-bound pass' cost. The
            // editor UI itself never moves: it stays native regardless of this slider.
            float rs = prefsDevice_ ? prefsDevice_->renderScale() : 1.0f;
            if (ImGui::SliderFloat("Render Scale", &rs, 0.25f, 1.0f, "%.2f") && prefsDevice_)
                prefsDevice_->setRenderScale(rs);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Renders the 3D scene at a fraction of the window's resolution, then\n"
                                  "upscales it back for display. The editor UI stays crisp either way.");
        }
        if (ImGui::CollapsingHeader("Viewport", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Show grid", &showGrid_);
            ImGui::Checkbox("Wireframe", &wireframe_);
            ImGui::SliderFloat("Fly speed (cm/s)", &flySpeed_, 20.0f, 20000.0f, "%.0f",
                               ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Look sensitivity", &lookSpeed_, 0.001f, 0.02f, "%.4f");
        }
        // --scroll-prefs-to-keybinds: a one-shot verification aid, same idea as --drawer's own
        // "put the UI in a state useful for a screenshot" precedent -- Preferences has grown to six
        // DefaultOpen sections, more than fit one screen at a normal window size, and there is no
        // human here to scroll. Fires once (consumes its own flag) so it never fights a person who
        // scrolls the window themselves.
        if (scrollPrefsToKeybinds_) { ImGui::SetScrollHereY(0.0f); scrollPrefsToKeybinds_ = false; }
        if (ImGui::CollapsingHeader("Keybinds", ImGuiTreeNodeFlags_DefaultOpen)) {
            keybinds_.drawPreferencesSection(dpi_);
        }
        ImGui::Separator();
        ImGui::TextDisabled("Preferences apply immediately and are saved for next time.");
        ImGui::TextDisabled("%s", editor::editorPrefsPath().c_str());
        ImGui::End();

        saveEditorPreferences();
    }

    // Draws the Project Settings window: a category sidebar beside the selected settings page.
    // Window > World Settings: the per-LEVEL settings, as opposed to Project Settings' per-project
    // ones. The split is the same one UE draws, and it is not cosmetic -- one project routinely holds
    // a menu level, a gameplay level and a test level that need different rules, and anything put in
    // the project forces all three to share.
    //
    // EVERY VALUE HERE EDITS SOMETHING THE LEVEL FILE ALREADY CARRIES. Nothing in this window is a
    // new parallel setting: the GameMode override writes OcWorldData::gameMode, the Player Start row
    // reports the SPAWN record's live marker, and the streaming rows edit each PCGVOLUME's own
    // `radius` and `samples` tokens. Adding a second, window-only copy of any of these is exactly
    // the two-sources-of-truth trap the Player Start marker was built to avoid.
    void buildWorldSettings() {
        if (!showWorldSettings_) return;
#if !AVER_MODULE_SCENE
        // WITHOUT THE SCENE MODULE THERE IS NO LEVEL to have settings for -- levelPath_/levelName_
        // are themselves scene-guarded. Saying so beats hiding the menu entry: the window exists in
        // every build, and a person who opens it deserves the reason it is empty rather than a menu
        // item that silently does nothing.
        if (ImGui::Begin("World Settings", &showWorldSettings_, ImGuiWindowFlags_NoDocking))
            ImGui::TextDisabled("This build has no scene module, so there is no level to configure.");
        ImGui::End();
        return;
#else

        const ImGuiViewport* mv = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(560.0f * dpi_, 480.0f * dpi_), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(mv->GetCenter().x, mv->GetCenter().y), ImGuiCond_FirstUseEver,
                                ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("World Settings", &showWorldSettings_, ImGuiWindowFlags_NoDocking)) {
            ImGui::End();
            return;
        }

        if (levelPath_.empty()) {
            ImGui::TextDisabled("No level is open.");
            ImGui::TextWrapped("World Settings edit the level file. Open or create a level first.");
            ImGui::End();
            return;
        }

        ImGui::TextDisabled("Level");
        ImGui::Separator();
        ImGui::Text("%s", levelName_.empty() ? "(untitled)" : levelName_.c_str());
        ImGui::TextDisabled("%s", levelPath_.c_str());
        ImGui::Dummy(ImVec2(0, 6.0f * dpi_));

        // ---- GameMode override ----
        ImGui::TextDisabled("Game Mode");
        ImGui::Separator();
        {
            // BY NAME, because that is what the file stores and what survives a restart: framework
            // class handles come from aver_fw_class_declare at runtime and are process-local, so a
            // number written into a level would mean something else next launch.
            char buf[128];
            const std::string& gm = levelHeader_.gameMode;
            std::snprintf(buf, sizeof buf, "%s", gm.c_str());
            ImGui::SetNextItemWidth(320.0f * dpi_);
            if (ImGui::InputText("GameMode Override", buf, sizeof buf)) {
                levelHeader_.gameMode = buf;
            }
            uiReg_.track("worldSettings.gameMode");

            // Says whether the name resolves, rather than leaving a typo to be discovered on Play.
            // A blank field is not an error -- it means "use the project default".
#if AVER_MODULE_FRAMEWORK
            if (levelHeader_.gameMode.empty()) {
                ImGui::TextDisabled("Empty -- the project's default GameMode applies.");
            } else {
                const int32_t c = aver_fw_class_find(levelHeader_.gameMode.c_str());
                if (c == 0) {
                    ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.2f, 1.0f),
                                       "No class named '%s' is declared.", levelHeader_.gameMode.c_str());
                    ImGui::TextDisabled("Compile .NET first, or check the spelling.");
                } else if ((aver_fw_class_get_flags(c) & AVER_FW_CLASS_GAME_MODE) == 0) {
                    ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.2f, 1.0f),
                                       "'%s' exists but is not a GameMode.", levelHeader_.gameMode.c_str());
                } else {
                    ImGui::TextDisabled("Resolved.");
                }
            }
#else
            ImGui::TextDisabled("This build has no framework module, so the name cannot be checked.");
#endif
        }
        ImGui::Dummy(ImVec2(0, 6.0f * dpi_));

        // ---- Player Start ----
        ImGui::TextDisabled("Player Start");
        ImGui::Separator();
        {
            Vec3 sp{}; f32 sy = 0.0f;
            if (playerStartTransform(sp, sy)) {
                ImGui::Text("(%.0f, %.0f, %.0f)  yaw %.0f", sp.x, sp.y, sp.z, sy);
#if AVER_MODULE_SCENE
                if (playerStart_ != scene::kInvalidEntity) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Select")) { sel_ = kSelScene; selEntity_ = playerStart_; }
                    uiReg_.track("worldSettings.selectPlayerStart");
                }
#endif
                ImGui::TextDisabled("Where the player spawns in. Move the marker to change it.");
            } else {
                ImGui::TextDisabled("None. Add one with Add > Player Start.");
            }
        }
        ImGui::Dummy(ImVec2(0, 6.0f * dpi_));

        // ---- Streaming, per density field ----
        ImGui::TextDisabled("World Streaming");
        ImGui::Separator();
        if (levelPcgVolumes_.empty()) {
            ImGui::TextDisabled("This level declares no PCGVOLUME, so nothing streams.");
        } else {
            // ONE ROW PER FIELD, because the radius is per field. A single level-wide "streaming
            // radius" would be exactly the setting this engine deliberately does not have: one
            // radius cannot serve a dense ground cover and a sparse canopy at once, which is why
            // PCGVOLUME carries its own.
            ImGui::TextDisabled("Radius is per density field, in 16m chunks. Cost is quadratic in it.");
            for (usize i = 0; i < levelPcgVolumes_.size(); ++i) {
                fmt::OcPcgVolume& v = levelPcgVolumes_[i];
                if (v.name == "Sky") continue;   // the cloud field; it streams nothing
                ImGui::PushID(static_cast<int>(i));
                ImGui::Text("%s", v.name.empty() ? "(unnamed)" : v.name.c_str());

                int radius = v.radiusChunks > 0 ? v.radiusChunks : 3;   // 0 = "runtime default (3)"
                ImGui::SetNextItemWidth(160.0f * dpi_);
                if (ImGui::SliderInt("Radius (chunks)", &radius, 1, 24)) {
                    v.radiusChunks = radius;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%.0fm", static_cast<f64>(radius) * 16.0);

                int samples = v.samplesPerAxis > 0 ? v.samplesPerAxis : 4;
                ImGui::SetNextItemWidth(160.0f * dpi_);
                if (ImGui::SliderInt("Samples / axis", &samples, 1, 32)) {
                    v.samplesPerAxis = samples;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%d candidates/chunk", samples * samples);
                ImGui::PopID();
                ImGui::Dummy(ImVec2(0, 4.0f * dpi_));
            }
            ImGui::TextDisabled("Takes effect when streaming is next enabled (Window > Chunk Streaming).");
        }

        ImGui::End();
#endif
    }

    void buildProjectSettings() {
        if (focusVoxi_ > 0) { showProjectSettings_ = true; --focusVoxi_; } // --project-settings
        if (!showProjectSettings_) return;

        const ImGuiViewport* mv = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(880.0f*dpi_, 560.0f*dpi_), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(mv->GetCenter().x, mv->GetCenter().y), ImGuiCond_FirstUseEver, ImVec2(0.5f,0.5f));
        if (!ImGui::Begin("Project Settings", &showProjectSettings_, ImGuiWindowFlags_NoDocking)) { ImGui::End(); return; }

        // settingsPage_: 0 Description, 1 Rendering>General, 2 >Global Illumination,
        // 3 >Ray Tracing (denoiser lives here, next to the rays it thins out), 4 >Path Tracing.
        ImGui::BeginChild("##categories", ImVec2(220.0f*dpi_, 0), ImGuiChildFlags_Borders);
        ImGui::TextDisabled("Project");
        ImGui::Indent();
        if (ImGui::Selectable("Description", settingsPage_==0)) settingsPage_=0;
        ImGui::Unindent();
        ImGui::TextDisabled("Engine");
        ImGui::Indent();
        ImGui::TextDisabled("Rendering");
        ImGui::Indent();
        if (ImGui::Selectable("General",               settingsPage_==1)) settingsPage_=1;
        if (ImGui::Selectable("Global Illumination",    settingsPage_==2)) settingsPage_=2;
        if (ImGui::Selectable("Ray Tracing",            settingsPage_==3)) settingsPage_=3;
        if (ImGui::Selectable("Path Tracing",           settingsPage_==4)) settingsPage_=4;
        ImGui::Unindent();
        ImGui::Unindent();
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_Borders);
        if (settingsPage_ == 0) {
            ImGui::TextUnformatted("Description");
            ImGui::Separator();
            if (project_.valid()) {
                ImGui::Text("Name        %s", project_.name.c_str());
                ImGui::Text("Author      %s", project_.author.empty() ? "(unset)" : project_.author.c_str());
                ImGui::Text("Engine      %s %s (this build: %.*s)",
                            project_.engineName.empty() ? "(unset)" : project_.engineName.c_str(),
                            project_.engineMinVersion.empty() ? "" : project_.engineMinVersion.c_str(),
                            (int)kEngineVersion.size(), kEngineVersion.data());
                ImGui::Text("Content     %s", project_.contentRoot.c_str());
                ImGui::Text("Start map   %s", project_.startMap.empty() ? "(unset)" : project_.startMap.c_str());
                ImGui::Separator();
                ImGui::TextDisabled("%s", project_.manifestPath.c_str());
                ImGui::Spacing();
                if (project_.hasRenderSettings())
                    ImGui::TextDisabled("This project states render settings; they were applied on open.");
                else
                    ImGui::TextDisabled("This project states no render settings yet.");
                ImGui::Spacing();

                ImGui::BeginDisabled(!projectDirty_);
                if (ImGui::Button("Save Project Settings", ImVec2(260.0f * dpi_, 0.0f))) {
                    std::string why;
                    projectSaveStatus_ = saveProjectManifest(&why) ? "Saved." : ("Save failed: " + why);
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(projectDirty_
                        ? "Writes the Rendering page's settings into the .ocproject.\nComments and any keys this build does not know are preserved."
                        : "Nothing has changed since the manifest was last read.");
                if (!projectSaveStatus_.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", projectSaveStatus_.c_str());
                }
                ImGui::TextDisabled("Rendering settings are the game's, so they go in the project.");
                ImGui::TextDisabled("The editor's own preferences are per-machine and do not.");
            } else {
                ImGui::TextDisabled("No project loaded. The editor runs fine without one; open or");
                ImGui::TextDisabled("create a project from the start screen to populate this page.");
            }
        } else {
#if AVER_MODULE_VOXI
            buildRenderingSettings(settingsPage_);
#else
            ImGui::TextUnformatted("Rendering");
            ImGui::Separator();
            ImGui::TextDisabled("Built without the Voxi render module (-DAVER_MODULE_VOXI=OFF).");
#endif
        }
        ImGui::EndChild();
        ImGui::End();
    }

#if AVER_MODULE_VOXI
    // A feature's status badge: green ready, amber not implemented, red unsupported.
    static void featureStatusBadge(aver::voxi::Renderer& vx, aver::voxi::Feature f) {
        using namespace aver::voxi;
        const Status st = vx.status(f);
        const ImVec4 col = st==Status::Ready ? ImVec4(0.45f,0.85f,0.45f,1)
                         : st==Status::NotImplemented ? ImVec4(0.95f,0.72f,0.25f,1)
                                                      : ImVec4(0.75f,0.35f,0.35f,1);
        ImGui::SameLine(); ImGui::TextColored(col, "[%s]", vx.statusText(f));
    }

    // Draws one of the Rendering page's sub-pages (General / Global Illumination / Ray Tracing /
    // Path Tracing) -- see the sidebar in buildProjectSettings for the hierarchy this belongs to.
    // Each feature reports its real status and is disabled when the renderer or the GPU cannot do
    // it. Reads and writes the WHOLE Settings struct regardless of which sub-page is showing, so
    // switching pages never drops a field only some other page's controls touch.
    void buildRenderingSettings(int page) {
        using namespace aver::voxi;
        Renderer& vx = Renderer::get();
        static const char* kPageTitle[] = {"", "General", "Global Illumination", "Ray Tracing", "Path Tracing"};
        ImGui::TextUnformatted(kPageTitle[page]);
        ImGui::SameLine(); ImGui::TextDisabled("(Voxi render module)");
        ImGui::Separator();

        Settings s = vx.settings();
        bool changed = false;
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);

        if (page == 1) {
            ImGui::TextUnformatted(Renderer::featureName(Feature::Msaa));
            const u32 mask = vx.deviceInfo().msaaMask;
            const u32 counts[4] = {1,2,4,8};
            const char* labels[4] = {"Off","2x","4x","8x"};
            for (int i=0;i<4;++i) {
                const bool ok = (mask & counts[i]) != 0;
                if (i) ImGui::SameLine();
                ImGui::BeginDisabled(!ok);
                if (ImGui::RadioButton(labels[i], static_cast<u32>(s.msaa)==counts[i])) { s.msaa=static_cast<Msaa>(counts[i]); changed=true; }
                ImGui::EndDisabled();
            }

            ImGui::Separator();
            const Status st = vx.status(Feature::MeshShaders);
            ImGui::TextUnformatted(Renderer::featureName(Feature::MeshShaders));
            featureStatusBadge(vx, Feature::MeshShaders);
            ImGui::BeginDisabled(st != Status::Ready);
            if (ImGui::Checkbox("Use mesh shaders", &s.meshShaders)) changed = true;
            ImGui::EndDisabled();
        }

        if (page == 2) {
            const Status st = vx.status(Feature::GlobalIllumination);
            ImGui::TextUnformatted(Renderer::featureName(Feature::GlobalIllumination));
            featureStatusBadge(vx, Feature::GlobalIllumination);
            ImGui::BeginDisabled(st != Status::Ready);
            int q = static_cast<int>(s.globalIllumination);
            const char* qs[] = {"Off","Low","Medium","High","Epic"};
            if (ImGui::Combo("Quality", &q, qs, 5)) { s.globalIllumination = static_cast<Quality>(q); changed = true; }

            int res = static_cast<int>(s.voxelResolution);
            const char* resLabels[] = {"64", "128", "256", "512"};
            const int resValues[] = {64, 128, 256, 512};
            int resIdx = res>=512 ? 3 : (res>=256 ? 2 : (res>=128 ? 1 : 0));
            if (ImGui::Combo("Voxel grid", &resIdx, resLabels, 4)) { s.voxelResolution = (u32)resValues[resIdx]; changed = true; }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Cubic edge of the GI volume -- memory and per-voxel GPU cost are\n"
                                   "both this NUMBER CUBED, so each step up is an 8x jump:\n"
                                   "  64  ~6 MB    128  ~50 MB    256  ~400 MB    512  ~3.2 GB\n"
                                   "Changing Quality above moves this to match its rung (Off/Low=64,\n"
                                   "Medium=128, High=256, Epic=512) unless you pick a value here\n"
                                   "yourself, which then overrides the tier's default.");
            if (ImGui::SliderFloat("GI intensity", &s.giIntensity, 0.0f, 4.0f)) changed = true;
            if (ImGui::SliderFloat("GI distance", &s.giMaxDistance, 10.0f, 20000.0f, "%.0f")) changed = true;
            ImGui::DragFloat3("Volume centre", &giCenter_.x, 0.5f);
            ImGui::DragFloat("Volume extent", &giExtent_, 0.5f, 1.0f, 100000.0f);
            ImGui::Checkbox("Debug: show voxel radiance", &giDebugView_);
            ImGui::EndDisabled();
        }

        if (page == 3) {
            const Status st = vx.status(Feature::RayTracing);
            ImGui::TextUnformatted(Renderer::featureName(Feature::RayTracing));
            featureStatusBadge(vx, Feature::RayTracing);
            ImGui::BeginDisabled(st != Status::Ready);
            int q = static_cast<int>(s.rayTracing);
            const char* qs[] = {"Off","Low","Medium","High","Epic"};
            if (ImGui::Combo("Quality", &q, qs, 5)) { s.rayTracing = static_cast<Quality>(q); changed = true; }

            ImGui::Spacing();
            ImGui::TextUnformatted("Sun shadow");
            ImGui::Separator();
            int rays = static_cast<int>(s.rtShadowRays);
            if (ImGui::SliderInt("Occlusion rays / pixel", &rays, 1, 32)) {
                s.rtShadowRays = static_cast<u32>(rays); changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How many rays a pixel that traces THIS frame casts toward the\n"
                                   "sun's disc. Linear in cost -- this is the knob to turn down\n"
                                   "first if ray tracing starts costing frames.");

            ImGui::Spacing();
            ImGui::TextUnformatted("Denoiser: temporal amortisation");
            ImGui::Separator();
            // Tile edge, not raw pixels-per-ray: VoxiRenderer::setPixelsPerRayTile only ever rounds
            // to a power of two, so offering anything else here would just be relabelled after the
            // fact. Options themselves are powers of two (1,2,4,8,16) so the SQUARE -- the pixel
            // count one ray actually covers -- is also always a power of two (1..256): the whole
            // point of the constraint, stated once here rather than re-derived at every call site.
            const int tiles[] = {1, 2, 4, 8, 16};
            const char* tileLabels[] = {"Off (1x1 -- every pixel, every frame)",
                                        "2x2 (4 pixels/ray)", "4x4 (16 pixels/ray)",
                                        "8x8 (64 pixels/ray)", "16x16 (256 pixels/ray)"};
            int tileIdx = 0;
            for (int i = 0; i < 5; ++i) if (tiles[i] == (int)s.rtPixelsPerRayTile) tileIdx = i;
            if (ImGui::Combo("Shadow amortisation", &tileIdx, tileLabels, 5)) {
                s.rtPixelsPerRayTile = static_cast<u32>(tiles[tileIdx]); changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("At N, one pixel in each NxN tile traces a fresh ray each frame;\n"
                                   "every other pixel reuses a reprojected history sample instead.\n"
                                   "Every pixel gets its own turn every N*N frames. Off is bit-for-\n"
                                   "bit identical to having no denoiser; larger tiles trade a real\n"
                                   "cut in rays traced for more frames of lag on fast-moving shadows.");

            // A SECOND, INDEPENDENT DENOISER, and the separate heading is the point: these two get
            // spoken about as one setting and they fail in opposite directions. The one above reuses
            // THIS pixel across TIME -- it converges beautifully while the camera is still and
            // collapses a penumbra to flat shadow the moment one moves. The one below averages
            // NEIGHBOURS within a single frame and keeps no history whatsoever, so there is nothing
            // to go stale and camera motion cannot poison it. They compose; they are not
            // alternatives, and turning one up is not a substitute for the other.
            ImGui::Spacing();
            ImGui::TextUnformatted("Denoiser: spatial filter");
            ImGui::Separator();
            int denoise = static_cast<int>(s.rtShadowDenoise);
            if (ImGui::SliderInt("Filter radius (px)", &denoise, 0, 3, denoise == 0 ? "Off" : "%d")) {
                s.rtShadowDenoise = static_cast<u32>(denoise); changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Averages a (2R+1)^2 neighbourhood of the shadow, weighting each\n"
                                   "neighbour by how well its depth agrees with this pixel's surface.\n"
                                   "At one ray per pixel the shadow term is a hard 0 or 1, so a\n"
                                   "penumbra comes out dithered rather than soft; this is what\n"
                                   "resolves it without paying for more rays. Keeps no history, so\n"
                                   "unlike amortisation above it costs nothing in lag under motion.");
            ImGui::EndDisabled();
        }

        if (page == 4) {
            // THIS COMBO IS NOW THE REAL CONTROL. It is voxi::Settings::pathTracing itself --
            // round-tripped through the .ocproject manifest (RENDER.PATHTRACING) and exposed to C#
            // scripting via aver_voxi_get/set_quality(PATH_TRACING), exactly like every other feature
            // on this page -- and it used to have NO relationship whatsoever to modules/render.pt:
            // Voxi.cpp hard-coded status(Feature::PathTracing) to Status::NotImplemented regardless
            // of hardware, so BeginDisabled below never unlocked and setSettings() clamped whatever
            // this held back to Off on every device, forever. status() now mirrors aver::pt::
            // PathTracer::init()'s own DXR-1.1/SM-6.5/DXC/compute gate field for field (see Voxi.cpp),
            // so this unlocks exactly when Aver.PathTracer's reference view can actually run, and the
            // value it holds is what syncPtSceneView() -- called from onUpdate(), never from here, see
            // that function's own comment for why -- reconciles PtSceneView's registration against.
            //
            // PtSceneView HAS NO QUALITY TIERS of its own: kAccumWidth/kAccumHeight/kMaxBounces/
            // kSamplesPerStep/kMaxSamples (PtSceneView.hpp) are fixed constants, never derived from a
            // rung the way voxelResolution derives from globalIllumination above. So every value but
            // Off means exactly the same thing here -- on -- until a real quality ladder exists for
            // it; stated honestly rather than inventing tiers that would do nothing.
            const Status st = vx.status(Feature::PathTracing);
            ImGui::TextUnformatted(Renderer::featureName(Feature::PathTracing));
            featureStatusBadge(vx, Feature::PathTracing);
            // ptSceneViewUnavailable_ is a RUNTIME signal PathTracer::init() itself raised (a DXC
            // compile failure, say) that the static device caps above did not predict -- see
            // syncPtSceneView()'s failure branch. Disabling on it too keeps this combo from claiming
            // a quality that is not actually running; it is NOT reset to Off automatically when that
            // happens (see syncPtSceneView()'s own comment), so a stale non-Off selection can sit
            // here, disabled, until the user picks Off explicitly or reopens the project.
            ImGui::BeginDisabled(st != Status::Ready || ptSceneViewUnavailable_);
            int q = static_cast<int>(s.pathTracing);
            const char* qs[] = {"Off","Low","Medium","High","Epic"};
            if (ImGui::Combo("Quality", &q, qs, 5)) {
                s.pathTracing = static_cast<Quality>(q);
                changed = true;
                // THE SEAM: this is the one place a UI event turns into a request for PtSceneView.
                // syncPtSceneView() performs the actual RHI registration next onUpdate(), never here.
                ptSceneViewWantEnabled_ = (s.pathTracing != Quality::Off);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Turns on Aver.PathTracer's reference view: a still-camera, brute-\n"
                                   "force render of the real scene through modules/render.pt.\n"
                                   "SUPPRESSES the raster view entirely while on. Sky and sun light\n"
                                   "only -- no CLight (point/spot/area) and no emissive term --\n"
                                   "static geometry only, flat albedo only, no denoiser -- see\n"
                                   "PtSceneView.hpp for the full list of what it deliberately does\n"
                                   "not do. No tiers yet: any value but Off means on.");
            ImGui::EndDisabled();

            if (ptSceneViewUnavailable_) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.75f,0.35f,0.35f,1), "[unavailable on this device]");
            } else if (ptSceneView_) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.45f,0.85f,0.45f,1), "[active]");
                ImGui::Text("%s, %u sample(s) accumulated",
                            ptSceneView_->sceneReady() ? "tracing" : "no static geometry captured yet",
                            ptSceneView_->samplesAccumulated());
            }
        }

        ImGui::PopItemWidth();
        ImGui::Separator();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("GPU: MSAA %ux, RT tier %u, shader model %u, mesh-shader tier %u, DXC %s",
                            vx.deviceInfo().maxMsaaSamples, vx.deviceInfo().rayTracingTier,
                            vx.deviceInfo().shaderModel, vx.deviceInfo().meshShaderTier,
                            vx.deviceInfo().dxcAvailable ? "yes" : "no");
        ImGui::TextDisabled("Scriptable from C# via aver_voxi_* (Aver.Scripting)");
        ImGui::PopTextWrapPos();

        if (changed) {
            // The requested settings reach the manifest before setSettings can clamp them.
            captureRenderSettingsFromUi(s);
            vx.setSettings(s);
        }
    }
#endif

    // Draws the viewport's overlay bars: view options on the left, transform tools, snapping and
    // camera speed on the right.
    void buildViewportOverlay() {
        if (vpW_ < 80.0f || vpH_ < 60.0f) return;
        const f32 pad = 8.0f*dpi_;
        const ImGuiWindowFlags f = ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|
                                   ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_AlwaysAutoResize|
                                   ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoSavedSettings|
                                   ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoNavFocus;

        ImGui::SetNextWindowPos(ImVec2(vpX_+pad, vpY_+pad), ImGuiCond_Always, ImVec2(0,0));
        ImGui::SetNextWindowBgAlpha(0.62f);
        ImGui::Begin("##vpbar_left", nullptr, f);
        if (dropButton("Perspective")) ImGui::OpenPopup("viewType");
        uiReg_.track("viewport.perspective");
        if (ImGui::BeginPopup("viewType")) {
            ImGui::Selectable("Perspective", true);
            const char* orthos[] = {"Top","Bottom","Left","Right","Front","Back"};
            for (const char* o : orthos) ImGui::Selectable(o, false, ImGuiSelectableFlags_Disabled);
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (dropButton(wireframe_ ? "Wireframe" : "Lit")) ImGui::OpenPopup("viewMode");
        if (ImGui::BeginPopup("viewMode")) {
            if (ImGui::Selectable("Lit", !wireframe_)) wireframe_=false;
            if (ImGui::Selectable("Wireframe", wireframe_)) wireframe_=true;
            ImGui::Selectable("Unlit", false, ImGuiSelectableFlags_Disabled);
            ImGui::Selectable("Detail Lighting", false, ImGuiSelectableFlags_Disabled);
#if AVER_MODULE_VOXI
            ImGui::Separator();
            if (ImGui::Selectable("Voxel Radiance (GI debug)", giDebugView_)) giDebugView_ = !giDebugView_;
#endif
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (dropButton("Show")) ImGui::OpenPopup("showFlags");
        uiReg_.track("viewport.show");
        if (ImGui::BeginPopup("showFlags")) {
            ImGui::Checkbox("Grid", &showGrid_);
            bool t=true; ImGui::Checkbox("Static Meshes", &t);
            ImGui::Checkbox("Atmosphere", &t);
            ImGui::EndPopup();
        }
        ImGui::End();

        const f32 icon = 26.0f*dpi_, caretW = 14.0f*dpi_, tiny = 2.0f*dpi_, gap = 6.0f*dpi_;
        ImGui::SetNextWindowPos(ImVec2(vpX_+vpW_-pad, vpY_+pad), ImGuiCond_Always, ImVec2(1,0));
        ImGui::SetNextWindowBgAlpha(0.62f);
        ImGui::Begin("##vpbar_right", nullptr, f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto toolBtn = [&](const char* id, int kind, bool active)->bool {
            return editor::toolButton(id, kind, active, icon, dpi_);
        };
        auto caretBtn = [&](const char* id, bool on)->bool {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton(id, ImVec2(caretW, icon));
            const bool hov = ImGui::IsItemHovered(), clk = ImGui::IsItemClicked();
            if (hov) dl->AddRectFilled(p, ImVec2(p.x+caretW,p.y+icon), IM_COL32(74,76,82,255), 3.0f);
            const ImVec2 c(p.x+caretW*0.5f, p.y+icon*0.5f); const f32 s=3.0f*dpi_;
            const ImU32 col = on ? IM_COL32(232,150,60,255) : IM_COL32(190,191,195,255);
            dl->AddTriangleFilled(ImVec2(c.x-s,c.y-s*0.6f), ImVec2(c.x+s,c.y-s*0.6f), ImVec2(c.x,c.y+s*0.8f), col);
            return clk;
        };

        // MODE FIRST, then that mode's tools. The switcher is always the leftmost thing in the
        // toolbar so "what am I editing" is answered before "with which tool" -- the two questions
        // were previously the same flat row, which is how a brush ended up sitting next to Rotate.
        {
            auto modeBtn = [&](const char* label, EditorMode m, bool enabled) {
                const bool on = mode_ == m;
                if (!enabled) ImGui::BeginDisabled();
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.79f, 0.47f, 0.16f, 1.0f));
                if (ImGui::Button(label)) setEditorMode(m);
                if (on) ImGui::PopStyleColor();
                if (!enabled) {
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("This level has no landscape section. Add a LANDSCAPE "
                                          "record, or pass --landscape <file.ocland>.");
                }
            };
            modeBtn("Select", EditorMode::Select, true);
            uiReg_.track("mode.select");
#if AVER_MODULE_LANDSCAPE
            ImGui::SameLine(0, gap);
            // Offered even with nothing loaded, but disabled and explaining why: a mode that simply
            // vanishes reads as a missing feature rather than an unmet precondition.
            modeBtn("Landscape", EditorMode::Landscape, landscapeLoaded_);
            uiReg_.track("mode.landscape");
#endif
            ImGui::SameLine(0, gap*2);
        }

#if AVER_MODULE_LANDSCAPE
        if (mode_ == EditorMode::Landscape) {
            if (toolBtn("##tSRaise", 4, sculptTool_==SculptTool::Raise)) sculptTool_=SculptTool::Raise;
            uiReg_.track("tool.sculptRaise");
            ImGui::SameLine(0, gap);
            if (toolBtn("##tSLower", 5, sculptTool_==SculptTool::Lower)) sculptTool_=SculptTool::Lower;
            uiReg_.track("tool.sculptLower");
            ImGui::SameLine(0, gap);
            if (toolBtn("##tSSmooth", 6, sculptTool_==SculptTool::Smooth)) sculptTool_=SculptTool::Smooth;
            uiReg_.track("tool.sculptSmooth");
            ImGui::SameLine(0, gap);
            if (toolBtn("##tSFlatten", 7, sculptTool_==SculptTool::Flatten)) sculptTool_=SculptTool::Flatten;
            uiReg_.track("tool.sculptFlatten");
            ImGui::SameLine(0, gap*2);
            // The brush settings live HERE, in the mode that owns them, rather than appearing and
            // disappearing from a shared row depending on which tool happened to be selected.
            char brushLbl[32]; std::snprintf(brushLbl, sizeof brushLbl, "Brush %.0f", sculptRadiusCm_);
            if (dropButton(brushLbl)) ImGui::OpenPopup("brushParams");
            ImGui::SameLine(0, gap);
            if (ImGui::Button(landscapeDirty_ ? "Save Terrain *" : "Save Terrain")) saveLandscape();
            uiReg_.track("landscape.save");
            if (ImGui::BeginPopup("brushParams")) {
                ImGui::SliderFloat("Radius (cm)", &sculptRadiusCm_, 50.0f, 5000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
                ImGui::SliderFloat("Strength (cm)", &sculptStrengthCm_, 5.0f, 2000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
                ImGui::EndPopup();
            }
        } else
#endif
        {
        if (toolBtn("##tSel", 0, tool_==Tool::Select)) tool_=Tool::Select;
        uiReg_.track("tool.select");
        ImGui::SameLine(0, gap);
        if (toolBtn("##tMove", 1, tool_==Tool::Move)) tool_=Tool::Move;
        uiReg_.track("tool.move");
        ImGui::SameLine(0, tiny); if (caretBtn("##cMove", snapMove_)) ImGui::OpenPopup("snapMove");
        ImGui::SameLine(0, gap);
        if (toolBtn("##tRot", 2, tool_==Tool::Rotate)) tool_=Tool::Rotate;
        uiReg_.track("tool.rotate");
        ImGui::SameLine(0, tiny); if (caretBtn("##cRot", snapRot_)) ImGui::OpenPopup("snapRot");
        ImGui::SameLine(0, gap);
        if (toolBtn("##tScl", 3, tool_==Tool::Scale)) tool_=Tool::Scale;
        uiReg_.track("tool.scale");
        ImGui::SameLine(0, tiny); if (caretBtn("##cScl", snapScale_)) ImGui::OpenPopup("snapScale");
        ImGui::SameLine(0, gap*2);
        if (ImGui::Button(worldSpace_ ? "World" : "Local")) worldSpace_ = !worldSpace_;
        }

        ImGui::SameLine(0, gap);
        char camLbl[32]; std::snprintf(camLbl, sizeof camLbl, "Cam %.0f", flySpeed_);
        if (dropButton(camLbl)) ImGui::OpenPopup("camSpeed");
        if (ImGui::BeginPopup("camSpeed")) {
            ImGui::SliderFloat("Speed (cm/s)", &flySpeed_, 20.0f, 20000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("snapMove")) {
            ImGui::Checkbox("Grid snap (position)", &snapMove_); ImGui::Separator();
            const f32 opts[] = {0.1f,0.25f,0.5f,1,2,5,10,50,100};
            for (f32 v : opts){ char b[24]; std::snprintf(b,sizeof b,"%g units", v); if (ImGui::Selectable(b, moveSnap_==v)){ moveSnap_=v; snapMove_=true; } }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("snapRot")) {
            ImGui::Checkbox("Angle snap (rotation)", &snapRot_); ImGui::Separator();
            const f32 opts[] = {1,5,10,15,30,45,90};
            for (f32 v : opts){ char b[24]; std::snprintf(b,sizeof b,"%gÂ°", v); if (ImGui::Selectable(b, rotSnap_==v)){ rotSnap_=v; snapRot_=true; } }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("snapScale")) {
            ImGui::Checkbox("Scale snap", &snapScale_); ImGui::Separator();
            const f32 opts[] = {0.05f,0.1f,0.25f,0.5f,1};
            for (f32 v : opts){ char b[24]; std::snprintf(b,sizeof b,"%g", v); if (ImGui::Selectable(b, scaleSnap_==v)){ scaleSnap_=v; snapScale_=true; } }
            ImGui::EndPopup();
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(vpX_+pad, vpY_+vpH_-pad), ImGuiCond_Always, ImVec2(0,1));
        ImGui::SetNextWindowBgAlpha(0.35f);
        ImGui::Begin("##vphint", nullptr, f | ImGuiWindowFlags_NoInputs);
#if AVER_MODULE_LANDSCAPE
        if (mode_ == EditorMode::Landscape)
            ImGui::Text("Landscape: %s  |  LMB paint  |  radius %.0f cm, strength %.0f cm  |  1-4 brushes, Tab to Select",
                        kSculptToolNames[(int)sculptTool_], sculptRadiusCm_, sculptStrengthCm_);
        else
#endif
        ImGui::Text("Select: %s  |  RMB fly (WASD/QE)  wheel speed  MMB pan  F focus  |  1-4 tools, Tab to Landscape", kToolNames[(int)tool_]);
        ImGui::End();
    }
#endif

    // Requests the probe pixel and the screenshot on a capture run, then reports what was read.
    // --probe-rel resolves against the live viewport rect, --probe is absolute, neither means centre.
    void captureCheck(Engine& e) {
        const u64 f = e.time().frame;
        const u64 sf = maxFrames_>8?maxFrames_-3:4;
        const u32 px_ = probeU_ >= 0.0f ? (u32)(vpX_ + vpW_ * probeU_)
                      : (probeX_ ? probeX_ : (u32)(vpX_ + vpW_*0.5f));
        const u32 py_ = probeV_ >= 0.0f ? (u32)(vpY_ + vpH_ * probeV_)
                      : (probeY_ ? probeY_ : (u32)(vpY_ + vpH_*0.5f));
        if (f==sf) {
            e.device()->requestCapture(px_, py_);
            capX_=px_; capY_=py_; capVpX_=vpX_; capVpY_=vpY_; capVpW_=vpW_; capVpH_=vpH_;
        }
        if (f>sf && !capDone_){
            const bool insideReq = (f32)capX_ >= capVpX_ && (f32)capX_ < capVpX_+capVpW_ &&
                                   (f32)capY_ >= capVpY_ && (f32)capY_ < capVpY_+capVpH_;
            const bool rectStable = capVpX_==vpX_ && capVpY_==vpY_ && capVpW_==vpW_ && capVpH_==vpH_;
            const char* tag = !insideReq ? "OUTSIDE-VIEWPORT"
                            : !rectStable ? "VIEWPORT-MOVED"
                                          : "in-viewport";
            f32 px[4]; if (e.device()->getCapture(px)) {
                AVER_INFO("[Sandbox] probe ({},{}) px ({:.2f},{:.2f},{:.2f}) raw ({},{},{}) viewport ({},{} {}x{}) {}",
                          capX_, capY_, px[0],px[1],px[2],
                          (int)(px[0]*255.0f+0.5f), (int)(px[1]*255.0f+0.5f), (int)(px[2]*255.0f+0.5f),
                          (int)capVpX_, (int)capVpY_, (int)capVpW_, (int)capVpH_, tag);
                if (!insideReq)
                    AVER_ERROR("[Sandbox] PROBE INVALID: ({},{}) is outside the 3D viewport ({},{} {}x{}) "
                               "-- the value above is editor chrome, not a shading result",
                               capX_, capY_, (int)capVpX_, (int)capVpY_, (int)capVpW_, (int)capVpH_);
                else if (!rectStable)
                    AVER_WARN("[Sandbox] PROBE SUSPECT: the viewport moved to ({},{} {}x{}) after the "
                              "capture was requested -- re-run before trusting the value above",
                              (int)vpX_, (int)vpY_, (int)vpW_, (int)vpH_);
            }
            if (!shot_.empty()){ std::vector<u8> img; u32 iw=0,ih=0;
                if (e.device()->getFrameImage(img,iw,ih)&&iw&&ih && stbi_write_png(shot_.c_str(),(int)iw,(int)ih,4,img.data(),(int)iw*4))
                    AVER_INFO("[Sandbox] screenshot: {} ({}x{})", shot_, iw, ih); }
            capDone_=true;
        }
    }

    u64 maxFrames_; bool headless_; std::string beamPath_, shot_;
    Tool initialTool_ = Tool::Select;
    std::vector<MeshObj> objects_;
    // The selection addresses either world: sel_ >= 0 is an objects_ index, -1 is nothing,
    // -2/-3/-4 are the sun/sky/post pseudo-entries, kSelScene means selEntity_ names a scene entity.
    static constexpr int kSelScene = -5;
    int sel_ = 1;
    // AvId, not scene::Entity: read and written from unguarded code (pick() across both worlds, the
    // sun/sky/post-process outliner rows, the applyXformTo MeshObj fallback) as a generic "nothing
    // selected" sentinel. scene::Entity is a bare alias for AvId, so this drops the dependency with
    // no behaviour change.
    AvId selEntity_ = kInvalidId;
    bool hideEditorScene_ = false;
    rhi::IDevice* prefsDevice_ = nullptr;   // borrowed, latched in buildUI
    std::string prefIdeName_;               // stored IDE name, pending the async scan that resolves it
    bool prefsLoaded_ = false;
    editor::AssetEditorHost assetEditors_;
    bool vsyncOffRequested_ = false;        // --no-vsync, pending a device to apply it to
    bool wantMeshReload_ = false;
    std::vector<u64> projectMeshIds_;      // what loadProjectMeshes added, so it can be undone
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

    // Copy/Duplicate's source, and what Paste rebuilds from. hasScene/hasObject are set exclusively
    // of each other by copySelection() -- mirrors the existing loose pairing of sel_/selEntity_
    // rather than a variant type for two cases already mutually exclusive by construction.
    struct EditorClipboard {
        bool hasScene = false;
#if AVER_MODULE_SCENE
        editor::EntitySnapshot sceneSnap;
#endif
        EditXform sceneXform{};
        bool hadBody = false;
        Vec3 bodyHalf{0, 0, 0};

        bool hasObject = false;
        MeshObj object{};
    };
    EditorClipboard clipboard_;

    // What chord means what command, defaults matching every hardcoded key this file used before
    // this registry existed. See EditorKeybinds.hpp for why it lives in its own file.
#if AVER_WITH_IMGUI
    editor::KeybindRegistry keybinds_;
#endif

    // Builds an outliner label from a surface name plus an ordinal: "M_Wall" -> "Wall 3". Falls back
    // to the asset's stem.
    std::string makeEntityLabel(const std::string& surface, const std::string& asset) {
        std::string base = surface;
        if (base.rfind("M_", 0) == 0) base.erase(0, 2);
        if (base.empty()) {
            const std::size_t slash = asset.find_last_of("/\\");
            base = slash == std::string::npos ? asset : asset.substr(slash + 1);
            const std::size_t dot = base.find_last_of('.');
            if (dot != std::string::npos) base.erase(dot);
        }
        if (base.empty()) base = "Entity";
        return base + " " + std::to_string(++labelCounts_[base]);
    }
    Tool tool_ = Tool::Select;

    // Which mode the viewport is in, and the brush the Landscape mode is holding. Both members are
    // UNGUARDED even though sculpting is AVER_MODULE_LANDSCAPE-only: the mode switch, the viewport
    // hint and the input dispatch all read them from unguarded code, and a member that disappears
    // under one configuration while its readers remain is the split-guard defect this file has been
    // bitten by repeatedly. With the module off, Landscape simply never becomes reachable.
    EditorMode mode_ = EditorMode::Select;
#if AVER_MODULE_LANDSCAPE
    SculptTool sculptTool_ = SculptTool::Raise;
#endif

    // True only when terrain editing is actually possible right now: the mode is Landscape AND a
    // section is loaded. Every "should this click sculpt" test goes through here rather than
    // checking the mode alone, so a mode left selected when a level without terrain loads cannot
    // paint into a section that is not there.
    bool editorModeIsLandscape() const {
#if AVER_MODULE_LANDSCAPE
        return mode_ == EditorMode::Landscape && landscapeLoaded_;
#else
        return false;
#endif
    }

    // Switching mode ends whatever the previous one was mid-way through. A drag that began as a
    // gizmo move and finishes as a brush stroke would apply one to the other's target.
    void setEditorMode(EditorMode m) {
        if (mode_ == m) return;
#if AVER_MODULE_LANDSCAPE
        if (m == EditorMode::Landscape && !landscapeLoaded_) {
            AVER_WARN("[Editor] Landscape mode needs a terrain section; this level has none");
            return;
        }
        sculpting_ = false;
        sculptCursorValid_ = false;
#endif
        dragging_ = false;
        mode_ = m;
        // Leaving Select with something selected is fine and even useful -- the selection is still
        // there when you come back -- but the gizmo must stop drawing, which it does because its
        // draw path tests the mode.
        AVER_INFO("[Editor] mode: {}", kEditorModeNames[static_cast<int>(m)]);
    }

    void toggleEditorMode() {
        setEditorMode(mode_ == EditorMode::Select ? EditorMode::Landscape : EditorMode::Select);
    }
    // Free-fly editor camera: position plus yaw/pitch.
    Vec3 camPos_{7.0f, 7.0f, 4.5f};
    f32 yaw_ = 0.0f, pitch_ = 0.0f, flySpeed_ = 800.0f, lookSpeed_ = 0.005f;   // cm/s
    bool flying_ = false;
    // Latched during the scene pass so the outline draws after every surface is down.
    Mat4 selectionOutline_{}; rhi::MeshHandle selectionMesh_ = 0; bool hasSelection_ = false;
    f32 sunColor_[3]={1.0f,0.96f,0.9f}, sunAmbient_=1.0f;
    f32 skyZenith_[3]={0.19f,0.42f,0.78f}, skyHorizon_[3]={0.72f,0.80f,0.90f};
    // A tint on the in-scattered sky (white = clear air), and an extinction per cm.
    f32 fogColor_[3]={1.0f,1.0f,1.0f}, fogDensity_=4e-6f;
#if AVER_MODULE_SCENE
    // OPT-IN (Height Fog panel, only shown while chunk streaming is on): recomputes fogDensity every
    // frame from the streaming load boundary instead of a density chosen once and left to drift out
    // of sync with a radius the user later changes. See the per-frame fog push (near setCamera) for
    // the derivation -- it solves averFogFactor's own k<=1e-8 branch (RHIShaders.cpp) for density,
    // it does not approximate it. Default OFF: this is a visibly foggier world, not a free win, and
    // must never become the silent default -- see the task this shipped with.
    bool matchFogToStreamRadius_ = false;
    f32  fogMatchTargetOpacity_ = 0.9f;   // opacity WANTED at the load boundary itself
#endif
    rhi::PostSettings post_{};
    // The authored sky, sun and air. Sole owner of the sun's direction.
    rhi::SkyAtmosphere sky_{};
    f32 cloudTime_ = 0.0f;   // seconds of accumulated wind
    bool capDone_=false;
    // The pixel requested and the viewport rect it was requested against, latched at request time.
    u32 capX_=0, capY_=0;
    f32 capVpX_=0, capVpY_=0, capVpW_=0, capVpH_=0;
    // editor viewport aids
    rhi::LineHandle gridMesh_=0;
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
    // The editor's Dear ImGui backend, constructed in onDeviceCreated (above) and installed into the
    // device non-owning, the same std::unique_ptr-owns/raw-pointer-on-the-device shape
    // averSrUpscaler_/edgeAaUpscaler_ use for IDevice::setUpscaler. UNLIKE those two, onShutdown below
    // deliberately does NOT reset() this early: the device's own uiShutdown() (called from
    // ~D3D12Device, which runs between onShutdown returning and EntryPoint.hpp's `delete app` --
    // see Engine::run) needs this object still alive when IT runs, not detached beforehand. Resetting
    // it in onShutdown would reproduce the exact dangling-raw-pointer bug averSrUpscaler_'s own
    // teardown-order comment describes, just earlier: the device's raw uiBackend_ would outlive the
    // object it points to for the remainder of the device's life, not just until destroyDevice.
    // Natural destruction order (this object dies only when SandboxApp itself does, well after the
    // device is gone) is what keeps this safe with no explicit detach at all.
    std::unique_ptr<rhi::d3d12::IUiBackend> uiBackend_;
#endif
    // 3D viewport rect, in backbuffer pixels. Latched by buildUI, consumed the next frame.
    f32 vpX_=0, vpY_=0, vpW_=1600, vpH_=900;
    bool dockBuilt_=false;   // one-shot DockBuilder layout (nothing is persisted to an ini)
    bool showProjectSettings_=false; // Edit > Project Settings window
    bool showWorldSettings_=false;   // Window > World Settings (per-LEVEL settings)
    bool showEditorPrefs_=false;     // Edit > Editor Preferences window
    bool scrollPrefsToKeybinds_=false;   // --scroll-prefs-to-keybinds, one-shot
    int  settingsPage_=1;            // 0 Description, 1 Rendering>General, 2 >GI, 3 >Ray Tracing, 4 >Path Tracing
    int  focusVoxi_=0;               // --project-settings: frames left to force the window open
    int  msaaOverride_=0;            // --msaa N: apply a sample count at startup
    // --depth-prepass: same-frame depth-only pass ahead of the ordinary opaque colour walk, so an
    // occluded fragment never reaches PSMainVoxi's shadow lookup/cone trace/fog. OFF (the default)
    // never calls IDevice::setDepthPrepassEnabled/drawMeshDepthPrepass/setNextDrawPrepassed at all --
    // see renderSceneEntities()'s own comment for the two-walk mechanism this drives.
    bool depthPrepassOverride_ = false;
// AND AVER_MODULE_SCENE, not just OCCLUSION -- and every OTHER `#if AVER_MODULE_OCCLUSION` in this
// file carries the same pair, deliberately. Two members below are keyed on scene::Entity
// (occlusionVisible_ and occlusionBoxEntities_), so a tree with OCCLUSION on and SCENE off had this
// block naming a type that does not exist; scripts/module-matrix.ps1's scene-off and all-off rows
// both failed there. Occlusion culls SCENE ENTITIES -- without a scene there is nothing for it to
// cull -- so requiring both is the honest condition rather than a workaround.
//
// THE PAIR HAS TO BE WRITTEN OUT AT ALL NINE SITES rather than just here. None of the occlusion
// blocks in this file is nested inside an AVER_MODULE_SCENE region (checked, all nine are
// top-level), so guarding only the members would leave their readers compiling against members
// that had vanished -- which is the exact split-guard shape this file has been bitten by
// repeatedly, just moved one step along.
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
    // --occlusion-cull: hierarchical-Z two-pass box culling (modules/occlusion). OFF (the default)
    // never calls occluder_ at all and never reorders the entity walk -- see renderSceneEntities's
    // own comment for the two-pass mechanism this drives, which mirrors depthPrepassOverride_'s
    // "own separate walk" shape but reorders IN PLACE rather than duplicating the walk, because the
    // draw logic it reuses (LOD selection, material binding, the GPU-cluster paths) is what
    // depthPrepassOverride_'s own walk deliberately does NOT replicate.
    bool occlusionCullEnabled_ = false;
    // NON-owning would be wrong here: this module has no registry of its own the way IRenderFeature
    // does, so SandboxApp owns the one instance for the run and destroys it in onShutdown.
    aver::occlusion::IOcclusionCuller* occluder_ = nullptr;
    // Per-entity "the pyramid could not prove this hidden, as of the last time it was tested" bit,
    // carried across frames -- see Occlusion.hpp's top comment on why this bookkeeping belongs to
    // the CALLER and not to the culler itself. Absent means "never tested" and defaults to visible
    // (see occlusionWasVisible below), so a freshly spawned entity is never missing from its first
    // frame on screen.
    std::unordered_map<scene::Entity, bool> occlusionVisible_;
    // This frame's reordering of [0, w.count()) so every PASS-1 (assumed-visible) index precedes
    // every PASS-2 one -- see renderSceneEntities's own comment for the two-pass shape this builds.
    // A MEMBER, not a local, purely to reuse its allocation frame to frame rather than reallocating
    // a several-thousand-entry vector every single frame on a scene this size.
    std::vector<u32> occlusionOrder_;
    // This frame's world AABBs, collected in a dedicated pre-walk ahead of the main entity loop --
    // see that pre-walk's own comment for why occluder_->testBatch() needs ALL of them (not just the
    // pass-2 subset) in one call, and why that means computing them once, up front, rather than
    // reusing whatever the main loop's OWN (per-pass-2-entity) box computation produces. Parallel
    // arrays, kept as members for the same reallocation-avoidance reason occlusionOrder_ is.
    std::vector<aver::occlusion::Aabb> occlusionBoxes_;
    std::vector<scene::Entity> occlusionBoxEntities_;
    std::vector<u8> occlusionResults_;
    // Diagnostics: accumulated since the process started, and the frame count they cover -- see the
    // periodic log line in renderSceneEntities for where these are read and reset.
    u64  occlusionCulledAccum_ = 0, occlusionTestedAccum_ = 0;
    u32  occlusionReportFrames_ = 0;
    bool occlusionWasVisible(scene::Entity e) const {
        const auto it = occlusionVisible_.find(e);
        return it == occlusionVisible_.end() || it->second;
    }
#endif
    int  giOverride_=0;              // --gi: GI quality to apply at startup
    bool giForceOff_=false;          // --no-gi: force it off, whatever the default is
    int  rtOverride_=0;              // --rt: ray tracing quality at startup
    bool rtForceOff_=false;          // --no-rt: force it off, whatever the default is
    int  rtRaysOverride_=0;          // --rt-rays N: sun occlusion rays per pixel (0 = flag not given)
    int  rtPixelsPerRayOverride_=0;  // --rt-pixels-per-ray N: shadow tile edge (0 = flag not given)
    int  rtShadowDenoiseOverride_=-1; // --rt-shadow-denoise N: spatial radius (-1 = flag not given)
    int  giUpdateIntervalOverride_=0; // --gi-update-interval N: GI revoxelise interval (0 = flag not given)
    f32  renderScaleOverride_=1.0f;  // --render-scale F: scene render resolution as a fraction of present, clamped [0.25,1]
#if AVER_MODULE_SR
    // --aversr LEVEL / the render-settings quality combo. Off (the default) is what a build with no
    // AverSR in it looks like: no render-scale change beyond whatever --render-scale itself asked
    // for, no SpatialUpscaler construction, nothing under the [AverSR] tag. See docs/AVERSR.md
    // "Quality levels" and this file's onInit()/applyAverSrQuality() for where it is actually read.
    aver::sr::Quality averSrQuality_ = aver::sr::Quality::Off;
    // Constructed lazily the first time a non-Off quality is applied (onInit() or the combo); never
    // rebuilt after that, only dropped back to null when quality returns to Off. `factory` (passed
    // to its constructor) must outlive every execute() call per AverSrSpatial.hpp's own contract --
    // satisfied here because it is the SAME rhi::IDevice::resources() the rest of the editor uses
    // for as long as the device exists.
    std::unique_ptr<aver::sr::SpatialUpscaler> averSrUpscaler_;
    // --edge-aa: constructs and hands the device a real aver::sr::FxaaResolve, the SAME way
    // averSrUpscaler_ just above does for SpatialUpscaler -- see ensureEdgeAaUpscaler(). OFF (the
    // default) never constructs one and never calls IDevice::setUpscaler for it, which is what keeps
    // a build with this flag unused bit-identical to one before this feature existed.
    //
    // SHARES ONE rhi::IDevice UPSCALER SLOT WITH AverSR: the interface (IDevice::setUpscaler) takes
    // one pointer, not a list, so only one of --aversr and --edge-aa can be the thing actually
    // running on any given frame. applyUpscalerSlot() below is the one place that decides which --
    // edge AA wins whenever both are requested, since render-scale upscaling (AverSR's job) and edge
    // AA (this flag's job) are two different reasons to want a full-screen resample and this task
    // only asked for the second one to exist. Not a limitation this task's own measurements hit:
    // none of the four configurations use --aversr.
    bool edgeAaEnabled_ = false;
    std::unique_ptr<aver::sr::FxaaResolve> edgeAaUpscaler_;
#endif
    bool frameTimeReport_=false;     // --frame-time: report the frame period, to price the above
    bool msOverride_=false;          // --ms: force the mesh shader geometry path
    u32  probeX_=0, probeY_=0;       // --probe X Y: absolute capture pixel (0 = viewport centre)
    f32  probeU_=-1.0f, probeV_=-1.0f;   // --probe-rel U V: a FRACTION of the viewport rect
    bool camOverride_=false;         // --cam X Y Z PITCH YAW: aim the viewport camera outright
    f32  camWobbleDeg_=0.0f;         // --cam-wobble DEG PERIOD: yaw amplitude, 0 = no motion
    i32  camWobblePeriod_=0;         // ...and its period in FRAMES; sin is 0 at every multiple
    f32  camWobbleBaseYaw_=0.0f;     // the yaw to swing about, latched on the first wobbled frame
    bool camWobbleBased_=false;
    Vec3 camPosOverride_{};
    f32  pitchOverride_=0.0f, yawOverride_=0.0f;   // radians, converted in setCamera
    bool useWarp_=false;             // --warp: run on the D3D12 software rasteriser
    std::string backendName_;   // --backend: which RHI backend to ask for first
    bool debugLayer_=false;          // --debug-layer: validate every graphics call (a real per-call tax)
    std::string scriptsDir_;         // --scripts <dir>: where to look for user script assemblies
    std::string spawnTestClass_;     // --spawn-test <ClassName>: headless actor-loop test trigger
    bool spawnTestDone_=false;       // the spawn is one-shot, done on the first frame scripts are ready
    int32_t spawnTestEntity_=0;      // the spawned test entity, destroyed a few frames later
    int spawnTestFrames_=0;          // frames since the test spawn, so the destroy is one-shot too
    bool playTest_=false;            // --play-test: headless begin_play -> tick -> end_play trigger
    bool playTestBegun_=false;       // begin_play has fired (one-shot, once a GameMode class is declared)
    int  playTestWait_=0;            // frames spent waiting for a GameMode class before giving up
    int  playTestFrames_=0;          // frames since begin_play, so the Stop is one-shot too
    editor::ProjectBrowser browser_;
    fmt::ProjectDesc project_;
    std::string projectPath_;        // <path>.ocproject given on the command line
    bool browserActive_=false;
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
#if AVER_MODULE_SCRIPTING
    scripting::ScriptHost scripts_;
#endif
    // The retained game UI's renderer. Heap-owned because create() may decline.
    aver::render::ui::UiRenderer* gameUi_ = nullptr;
    bool skinTest_ = false;       // --skin-test: GPU skinning against its CPU reference, then exit
    std::unique_ptr<aver::render::SkinSelfTest> skinSelfTest_;
    bool skinDrawTest_ = false;   // --skin-draw-test: does the RASTERISER read the skinned buffer
    std::unique_ptr<aver::editor::SkinDrawTest> skinDraw_;
#if AVER_MODULE_SCENE
    // The scene join: gives every entity with a CSkeletalMesh its own posed mesh. Null when the
    // skinning shader would not compile, in which case skinned entities simply draw at rest.
    std::unique_ptr<aver::render::SkinnedScene> skinnedScene_;
#endif
    std::string skinSceneDir_;    // --skin-scene-test <dir>: where the cooked rig lives
    std::unique_ptr<aver::editor::SkinSceneTest> skinScene_;
    bool particleTest_ = false;   // --particle-test: a dust cloud straddling an opaque occluder, so
                                   // the transparent pass's own depth test shows in one screenshot
    bool noParticleGi_ = false;   // --no-particle-gi: see setNoParticleGi's own comment
    // VERIFICATION-ONLY INSTRUMENTATION (not part of the particles slices): --particle-stress <N> <M>
    // spawns N grid-arranged dust-style emitters, each capped at M particles, purely to price the
    // system at a chosen emitter/particle count for the parity-and-price adversarial pass. See
    // setParticleStress's own comment.
    int  particleStressEmitters_ = 0;
    int  particleStressMaxParticles_ = 0;
    bool particleStressSecondEmitter_ = false;   // --particle-stress2: diag, see its own comment
    // Same purpose: direct CPU wall-clock timing around particles::particleSystem().tick(), since no
    // existing --frame-time/GPU-marker path measures CPU simulation separately from the GPU draw.
    // Accumulated every frame the particle module is compiled in and printed once at shutdown; the
    // std::chrono::steady_clock call itself costs single-digit nanoseconds and is not gated behind a
    // flag because leaving it always-on is what proves it did not skew any of this session's own
    // earlier probes (see the price section of this pass's report for that check).
    f64  particleTickAccumSec_ = 0.0;
    u64  particleTickFrames_ = 0;
    bool reflTest_ = false;       // --refl-test: are ray-traced reflections global?
    bool furnaceTest_ = false;    // --furnace-test: does the shading model conserve energy?
    bool furnaceSun_ = false;     // --furnace-sun: the variant where only the DIRECT term is lit
    f32  sunAngle_ = -1.0f;       // --sun-angle: negative leaves the sky's own value alone
    bool ptFurnaceTest_ = false;  // --pt-furnace: the same question asked of the path tracer
    std::unique_ptr<aver::pt::PtFurnaceTest> ptFurnace_;
    // --pt-scene: the path tracer pointed at the real scene. ptSceneViewWantEnabled_ below is the
    // one flag that matters now (see its own comment) -- there used to be a separate ptSceneViewFlag_
    // here too, but nothing ever read it once syncPtSceneView() took over registration, so it was
    // dead weight kept only by the refactor that introduced the want-flag. Removed.
    std::unique_ptr<aver::pt::PtSceneView> ptSceneView_;
    // ptSceneViewWantEnabled_ is the REQUESTED state (set by --pt-scene at startup, or by the
    // editor's own Path Tracing settings-page Quality combo -- voxi::Settings::pathTracing != Off --
    // at any later frame); ptSceneView_ != nullptr is the ACTUAL one. syncPtSceneView() reconciles
    // the two -- see its own comment for why it only ever runs from onUpdate(), before the frame's
    // beginFrame(). Deliberately NOT itself a read of voxi::Settings::pathTracing (see
    // syncPtSceneView()'s failure branch): this flag, and PT's registration, must keep working with
    // AVER_MODULE_VOXI off, since --pt-scene has never needed Voxi and still must not.
    bool ptSceneViewWantEnabled_ = false;
    bool ptSceneViewUnavailable_ = false;   // init() refused once this session -- stop re-asking
    // --pt-scene-toggle-on/--pt-scene-toggle-off [N]: VERIFICATION ONLY. Simulates a human flipping
    // the Path Tracing settings-page Quality combo N frames into a bounded run, so a --frames capture can
    // prove the RUNTIME toggle (register/unregister mid-session, not just --pt-scene's register-
    // before-frame-1 path) without a human clicking anything -- same idiom and reason as
    // chunkStreamAutoFrames_/droneAutoFrames_ above. Two independent countdowns from process start,
    // not "N frames after the ON one", so the caller picks values (e.g. on=5, off=15) rather than
    // this class reasoning about their order.
    int ptSceneToggleOnAutoFrames_ = 0;
    int ptSceneToggleOffAutoFrames_ = 0;
    std::unique_ptr<aver::editor::ReflTest> refl_;
    int  reflBeaconIndex_ = -1;   // which objects_ entry the schedule shows and hides
    rhi::MeshHandle unitCubeMesh_ = 0;   // the editor's own unit cube, half-extent 1
    bool showUiDemo_ = false;
    std::string matSaveStatus_;   // what the last 'Save to C#' did
    unsigned centralDock_ = 0;    // the dock node an opened asset editor lands in
    bool levelFocused_ = true;    // the Level tab holds the keyboard
    bool levelHovered_ = true;    // the cursor is over the Level tab and it is topmost there
    bool inputProbe_ = false;
    bool levelVisible_ = true;    // the Level tab is the selected tab
    std::string openAsset_;
    std::string lastOpenAssetPath_;  // last path --open-asset opened; openAsset_ itself is cleared
                                      // once consumed, so --graph-select needs its own copy to find it
    std::string graphSelectNode_;
    std::string graphTab_; // --graph-select <nodeId>; see setGraphSelectNode's own comment

    editor::ProjectUpgrade pendingUpgrade_;
    bool        upgradeAsked_ = false;
    bool        exitPrompt_ = false;      // the unsaved-changes modal is up
    std::string exitPromptError_;         // why a "Save all" attempt failed
    // Every PCGVOLUME the loaded level carried, kept verbatim so a save cannot drop them.
    std::vector<fmt::OcPcgVolume> levelPcgVolumes_;
    // The loaded level's own header, placements and PCG volumes stripped.
    //
    // THE SAME REASONING AS levelPcgVolumes_, GENERALISED. saveLevel used to build a fresh
    // OcWorldData from the editor's state, so every field the editor does not model was reset to a
    // default on write: SPAWN was deleted outright, BUILD went back to 0, ID was recomputed from
    // NAME, and the sun's `lux` reverted to 100000 however the level had authored it. Starting the
    // save from what the file actually said, and overwriting only what the editor genuinely owns,
    // fixes all of those at once -- and keeps fixing them for any field added to the format later,
    // which enumerating them one by one would not.
    fmt::OcWorldData levelHeader_;
    // Whether each level entity's placement said `nocollide`. There is NO component for this: it is
    // a load-time instruction and nothing on the entity records it afterwards, so without this the
    // save forced `collide = true` on everything and `nocollide` never survived a round trip.
    std::unordered_map<u32, bool> entityCollide_;
    std::string upgradeStatus_;
    f32         upgradeStatusAge_ = 0.0f;   // seconds since it was set; see setUpgradeStatus
    f32  uiDemoHealth_ = 0.72f, uiDemoStamina_ = 0.44f, uiDemoScroll_ = 0.0f, uiDemoClock_ = 0.0f;

    rhi::MeshHandle cubeMesh_=0; u32 cubeTris_=0; int spawnCount_=0;
    // The built-in look for a named surface with no material asset behind it.
    struct SurfaceLook { f32 col[3]; f32 metallic; f32 roughness; };
    std::unordered_map<i32, SurfaceLook> surfaceLooks_;
    // fnv1a64(content-relative path) -> absolute path.
    //
    // OUTSIDE THE PBR GUARD, and it was inside. Its original purpose was `{guid:...}` texture
    // references, which is a material concern -- but resolveAnimAsset and resolveSceneMesh read it
    // too, and both are needed to put a MESH in the world, which has nothing to do with whether the
    // material system is compiled in.
    std::unordered_map<u64, std::string> contentIndex_;
#if AVER_MODULE_PBR
    rhi::IResourceFactory* textureFactory_ = nullptr;   // cached: the resolver is a static callback
    // Surface name -> its .ocmat's material. 0 is a cached negative, not a miss to retry.
    std::unordered_map<std::string, pbr::MaterialHandle> materialAssets_;
    // The same answer keyed by the token the scene interns, which is what a CMeshRenderer carries.
    std::unordered_map<i32, pbr::MaterialHandle> surfaceMaterials_;
#endif

    // ---------------- landscape (opt-in; --landscape <path>, or <levelname>.ocland beside the level) ----------------
    // Hosts ONE open .ocland section: render, level-reference, AND sculpt (raise/lower/smooth/
    // flatten via the sculpt tools below -- see docs/LANDSCAPE_EDITOR.md's slice 0 for what is still
    // NOT here: no .ocworld-level reference record, no collision, no asset-editor tab). Independent
    // of AVER_MODULE_SCENE -- a section is not an ECS entity, and LandscapeRenderer::draw() calls
    // IDevice::drawMesh directly, the same hand-rolled pattern the objects_ loop above uses, not an
    // IRenderFeature (LandscapeRenderer implements none of its virtuals) and not
    // GameRender::drawWorld/CMeshRenderer (a section has no CMeshRenderer to route through). See that
    // doc's section 3 and section 6 item 6.
    //
    // UNGUARDED, matching chunkStreamAutoFrames_'s own reasoning right below: setLandscapePath() must
    // compile with the module off (command-line parsing is unconditional), so the field it writes
    // must exist unconditionally too. Everything that actually READS it is inside
    // `#if AVER_MODULE_LANDSCAPE`.
    std::string landscapeCliOverride_;
#if AVER_MODULE_LANDSCAPE
    fmt::OcLandData landscapeData_;
    landscape::LandscapeTree landscapeTree_;
    // Heap-owned so unloadLandscape() can destroy and recreate it independently of the section data,
    // mirroring LandscapeRenderer's own forget-then-discard lifecycle (LandscapeRenderer.hpp:46-58).
    std::unique_ptr<landscape::LandscapeRenderer> landscapeRenderer_;
    // The material name the level's LANDSCAPE record named, held as TEXT rather than a resolved
    // handle: the material system is not necessarily ready when the level parses, and re-resolving
    // from the name is what lets applyLandscapeSurfaceToAll() be called again later without caring
    // which of the two finished first.
    std::string landscapeMaterial_;
    // World centimetres per texture tile for the landscape mesh's baked UVs, taken from the resolved
    // LANDSCAPE material (see applyLandscapeSurface). Unconditional and defaulted to the same 1000
    // LandscapeRenderer::draw() itself defaults to, so a build without PBR/VOXI -- where nothing ever
    // assigns it -- draws exactly as it did before this member existed.
    f32 landscapeUvTilingCm_ = 1000.0f;
    // False until the tiling above came from a material the library actually considers valid, rather
    // than from MaterialSystem's fallback. Separate from hasSurfaceBinding() on purpose: the binding
    // latches on the FIRST successful apply, which can be a frame where the material exists but has
    // not been drained into the system yet -- so a single latch would freeze the tiling at the
    // fallback's 200cm and never look again.
    bool landscapeUvTilingResolved_ = false;
    bool landscapeLoaded_ = false;
    std::string landscapePath_;   // the section actually resident; empty when none is
    bool landscapeDirty_ = false; // true once a sculpt has touched landscapeData_ since the last save
    // The static body the terrain collides through, or -1. Guarded at every USE rather than here:
    // the member is unconditional so the declaration cannot go out of scope from under a call site
    // guarded differently -- the split-guard defect this file has been bitten by repeatedly.
    i32 landscapeBody_ = -1;

    // Sculpt tool state. Radius/strength are shared across all four brush modes -- the same "one
    // knob set, the mode picks what it means" shape the transform tools' snap popups already use.
    f32 sculptRadiusCm_ = 500.0f;
    f32 sculptStrengthCm_ = 150.0f;
    bool sculpting_ = false;              // LMB down, over the terrain, with a sculpt tool active
    f32 sculptFlattenTargetCm_ = 0.0f;    // captured once per stroke -- see BrushParams::flattenTargetCm
    bool sculptCursorValid_ = false;      // true when this frame's cursor ray actually hit the section
    Vec3 sculptCursor_{0, 0, 0};          // world hit point, for the brush-radius ring and the next tick
    rhi::LineHandle brushRing_ = 0;       // a unit ring in the XY plane -- landscape heights run +Z

    // ---------------- the ring: procedural tiles past the authored section's own rim ----------------
    // The authored section (landscapeData_/landscapeTree_/landscapeRenderer_ above) is ALWAYS tile
    // (0, 0) of a grid the size of its own extentCm(), centred on wherever the level placed it -- see
    // TerrainTile.hpp. It keeps its own storage and every sculpt/raycast/collision/save code path
    // above untouched; this is purely the ADDITIONAL tiles that make the world not stop dead at its
    // edge. A tile here is never authored and never sculpted: it is regenerated from terrainHeightAt
    // whenever it re-enters residency, which is why there is nothing to save for it.
    struct LandscapeRingTile {
        fmt::OcLandData data;
        landscape::LandscapeTree tree;
        std::unique_ptr<landscape::LandscapeRenderer> renderer;
    };
    std::unordered_map<landscape::TileCoord, LandscapeRingTile> landscapeRingTiles_;
    landscape::TerrainNoiseParams landscapeNoiseParams_;   // shared by every ring tile AND heightSource
    static constexpr i32 kLandscapeRingRadius = 1;         // tiles each side of the camera's tile: 3x3
    static constexpr u32 kLandscapeMaxSectionsResident = (2 * kLandscapeRingRadius + 1) *
                                                          (2 * kLandscapeRingRadius + 1);   // 9
    // docs/LANDSCAPE_EDITOR.md's blocker 9: the renderer's transient constant ring is a SHARED,
    // fixed 1 MiB no matter how many sections are resident, so the single section's own long-safe
    // defaults (192 draws, 512 cached meshes) are a TOTAL from here on -- split evenly across the
    // largest window this ring can ever hold, never handed to each resident tile whole. Computed once,
    // statically, rather than redivided every time the resident count changes: reconstructing the home
    // tile's LandscapeRenderer (its cap is fixed at construction) to match a moving divisor would throw
    // away its whole mesh cache on every tile crossing, which is a worse cost than the modest
    // under-use of an empty ring slot's unspent share.
    static constexpr u32 kLandscapeMaxDrawsPerTile =
        192u / kLandscapeMaxSectionsResident;
    static constexpr u32 kLandscapeMaxResidentNodesPerTile =
        512u / kLandscapeMaxSectionsResident;
    landscape::TileCoord landscapeLastCameraTile_{};
    bool landscapeLastCameraTileValid_ = false;
#endif

#if AVER_MODULE_SCENE
    // Turns chunk streaming on or off around the editor camera. OPT-IN: nothing in modules/world's
    // generator or region files is touched until a user flips Window > Chunk Streaming, so an
    // ordinary project opens exactly as it always did.

    // The exact inverse of averFogFactor's k<=1e-8 branch (RHIShaders.cpp:558-566): that function
    // computes opacity(d) = 1 - exp(-density*d), so this solves the SAME expression backwards for
    // density given a target opacity at a known distance. Not an approximation of the shader --
    // level-authored fog always takes that branch, because OcWorld.hpp/.cpp has no fields for
    // fogFalloff or fogStart at all (only density and color parse from a FOG record), so k and start
    // are always 0 for anything a level can author.
    static f32 fogDensityForOpacityAt(f32 distanceCm, f32 targetOpacity) {
        if (distanceCm <= 1.0f) return 0.0f;
        const f32 t = targetOpacity < 0.01f ? 0.01f : (targetOpacity > 0.999f ? 0.999f : targetOpacity);
        return -std::log(1.0f - t) / distanceCm;
    }

    // would silently keep scattering onto the heights the terrain had when streaming started. It is
    // Folds one additional field's stream stats into the running total. Counters add; `totalLoads`
    // adds too because it is a lifetime counter per world.
    //
    // GUARDED: world::StreamStats and scene::Entity do not exist with AVER_MODULE_SCENE=OFF, and
    // every caller of these two is inside a SCENE guard already. Missing it here is what broke the
    // scene-off row of scripts/module-matrix.ps1 -- which is the only thing that checks this, and
    // is exactly why it exists.
#if AVER_MODULE_SCENE
    static void accumulateStreamStats(world::StreamStats& into, const world::StreamStats& add) {
        into.residentChunks   += add.residentChunks;
        into.residentEntities += add.residentEntities;
        into.loadedThisUpdate += add.loadedThisUpdate;
        into.evictedThisUpdate += add.evictedThisUpdate;
        into.entitiesIn       += add.entitiesIn;
        into.entitiesOut      += add.entitiesOut;
        into.pendingLoads     += add.pendingLoads;
        into.failedLoads      += add.failedLoads;
        into.totalLoads       += add.totalLoads;
    }

    // True when any resident density field owns `e`. The World Outliner and the save path both use
    // this to tell streamed entities from authored ones, so it MUST see every world -- a streamed
    // entity that no world claims would be offered for editing and written into the level file.
    bool anyChunkWorldOwns(scene::Entity e) const {
        if (chunkWorld_ && chunkWorld_->owns(e)) return true;
        for (const auto& extra : chunkWorldsExtra_)
            if (extra && extra->owns(e)) return true;
        return false;
    }
#endif

    void setChunkStreamingEnabled(bool on) {
        if (on == (chunkWorld_ != nullptr)) return;

        if (!on) {
            std::vector<i32> freed;
            world::StreamStats last = chunkWorld_->stats();
            chunkWorld_->shutdown(scene::World::instance(), freed);
            // Every additional field is torn down in the same pass and into the SAME `freed` list --
            // those bodies are as real as the primary's, and leaving them would leak a physics body
            // per streamed collider each time streaming is toggled.
            for (auto& extra : chunkWorldsExtra_) {
                if (!extra) continue;
                accumulateStreamStats(last, extra->stats());
                extra->shutdown(scene::World::instance(), freed);
            }
            chunkWorldsExtra_.clear();
            scene::World::instance().flush();
#if AVER_MODULE_PHYSICS
            for (const i32 b : freed) if (b >= 0) aver_phys_remove_body(b);
#endif
            chunkWorld_.reset();
            chunkStreamHaveLastPos_ = false;
            chunkStreamStats_ = world::StreamStats{};
            AVER_INFO("[ChunkWorld] streaming disabled -- {} chunk(s) / {} entities released",
                      last.residentChunks, last.residentEntities);
            return;
        }

        if (!project_.valid()) {
            AVER_WARN("[ChunkWorld] cannot enable streaming: no project is open");
            return;
        }

        // ---- ONE ChunkWorld PER DECLARED DENSITY FIELD ----
        //
        // A ChunkWorld carries its own StreamSettings, and loadRadius is what decides how far the
        // world is actually populated. One field means one radius for everything, and one radius
        // cannot serve a dense floor and a sparse canopy at once: at the shipped 3 the scatter ends
        // 48 m out while the camera sees to the horizon, so the world stops at a visible ring with
        // bare terrain beyond it. Raising that single radius is not the fix -- the palette is
        // dominated by ground cover, and radius 6 quadruples the chunk count for triangles that are
        // invisible at that distance.
        //
        // Several fields, each with its own radius, seed, feature size and species, is the fix:
        // canopy at radius 10 with 3 samples/axis, floor at 3 with 12. Cost then scales with what is
        // visible at each distance instead of with the largest radius any one species needs.
        //
        // ONE WORLD PER FIELD rather than one generator holding several specs, because that is the
        // SMALLER change -- ChunkGenerator's sampling core is reproducibility-pinned and its header
        // is explicit about protecting it.
        std::vector<const fmt::OcPcgVolume*> fields;
        for (const fmt::OcPcgVolume& pv : levelPcgVolumes_)
            if (pv.name != "Sky") fields.push_back(&pv);

        // No non-Sky volume still builds exactly ONE world on GeneratorSettings' shipped defaults --
        // the behaviour before any of this, and what the single-species cube fallback relies on.
        const usize fieldCount = fields.empty() ? usize{1} : fields.size();
        const bool multi = fieldCount > 1;

        // A species naming a volume the level does not declare would otherwise scatter nowhere, in
        // silence. Reported once per bad name and folded into the primary rather than dropped: a
        // typo in one SCATTER line must not delete that species from the world.
        for (const fmt::OcScatterSpecies& sp : levelHeader_.scatterSpecies) {
            if (sp.volume.empty()) continue;
            bool found = false;
            for (const fmt::OcPcgVolume* pv : fields) if (pv->name == sp.volume) { found = true; break; }
            if (!found)
                AVER_WARN("[ChunkWorld] SCATTER '{}' names volume '{}', which this level does not "
                          "declare; it will scatter in the first field instead",
                          sp.meshPath, sp.volume);
        }

        std::vector<std::unique_ptr<world::ChunkWorld>> built;
        for (usize fi = 0; fi < fieldCount; ++fi) {
            const fmt::OcPcgVolume* v = fields.empty() ? nullptr : fields[fi];

            // Which species this field places. An UNNAMED species goes to the first field, which is
            // exactly where every species went before the `volume` token existed -- so a level that
            // never mentions volumes produces one world with the whole palette, as before. A species
            // naming a volume that does not exist lands here too (warned above).
            std::vector<fmt::OcScatterSpecies> mine;
            for (const fmt::OcScatterSpecies& sp : levelHeader_.scatterSpecies) {
                bool named = false;
                if (!sp.volume.empty())
                    for (const fmt::OcPcgVolume* pv : fields) if (pv->name == sp.volume) { named = true; break; }
                if (named) { if (v && sp.volume == v->name) mine.push_back(sp); }
                else if (fi == 0)                            mine.push_back(sp);
            }
            // A field with no species would generate the fallback cube everywhere. Skip it: a level
            // may declare a field for something other than scatter (a cave mask, a moisture map).
            if (mine.empty()) {
                AVER_INFO("[ChunkWorld] field '{}' has no SCATTER species; not streamed",
                          v ? v->name : std::string("<none>"));
                continue;
            }

            auto cw = std::make_unique<world::ChunkWorld>();
            world::ChunkWorldSettings cwSettings;

            // Beside Content and Binaries, not inside either: generated/streamed state, not authored
            // content, so it must never appear in the Content Browser or be packaged as an asset.
            //
            // PER FIELD ONLY WHEN THERE IS MORE THAN ONE. Region files are keyed by chunk coordinate,
            // so two worlds sharing a directory would write each other's chunks -- they must not.
            // A single-field level keeps the plain "Chunks" path it has always used, so no existing
            // project's streamed state is orphaned by this change.
            cwSettings.worldDir = project_.dir + "\\Chunks";
            if (multi) cwSettings.worldDir += "\\" + (v && !v->name.empty() ? v->name
                                                                            : std::to_string(fi));

            // The scatter palette comes from the LEVEL's own SCATTER records; the editor has no
            // opinion about what a world scatters. A field whose species all fail validation streams
            // nothing rather than falling back to the cube.
            {
                std::vector<std::string> scatterErrors;
                if (!world::buildScatterPalette(mine, project_.contentDir(),
                                                cwSettings.generator.palette, scatterErrors)) {
                    for (const std::string& e : scatterErrors)
                        AVER_WARN("[ChunkWorld] {}", e);
                }
            }

            // The field's own noise parameters. COVERAGE MAPS TO threshold, NOT to the generator's
            // pcg::InfiniteSpec::coverageFloor/coverageBias -- those are pinned to 0/1 inside
            // GeneratedChunkSource::setSettings on purpose, because sampleInfinite's final
            // pow(remapped, bias) is the one operation in it IEEE 754 does not pin across libm
            // implementations, and this generator's reproducibility depends on never calling it with
            // bias != 1. coverageFloor and threshold mean the same thing in different words.
            if (v) {
                cwSettings.generator.worldSeed = static_cast<u64>(static_cast<u32>(v->seed));
                if (v->cellSizeCm > 0.0) cwSettings.generator.featureSizeCm = static_cast<f32>(v->cellSizeCm);
                if (v->octaves > 0) cwSettings.generator.octaves = static_cast<u32>(v->octaves);
                {
                    const f64 t = v->coverageFloor < 0.0 ? 0.0
                                                          : (v->coverageFloor > 1.0 ? 1.0 : v->coverageFloor);
                    cwSettings.generator.threshold = static_cast<f32>(t);
                }
                // Clamped rather than trusted: cost is quadratic in this and the file is authored by
                // hand, so a stray digit would generate millions of entities per chunk.
                if (v->samplesPerAxis > 0) {
                    constexpr i32 kMaxSamplesPerAxis = 64;   // 4096 candidates in one chunk
                    const i32 n = v->samplesPerAxis > kMaxSamplesPerAxis ? kMaxSamplesPerAxis
                                                                          : v->samplesPerAxis;
                    if (n != v->samplesPerAxis)
                        AVER_WARN("[ChunkWorld] PCGVOLUME '{}' asks for {} samples per axis; clamped to {}",
                                  v->name, v->samplesPerAxis, n);
                    cwSettings.generator.samplesPerAxis = static_cast<u32>(n);
                }
                // How far this field streams. Clamped for the same reason, and evictRadius is raised
                // with it: ChunkStreamer.hpp requires evictRadius > loadRadius or the boundary
                // thrashes, and it enforces that rather than trusting the caller.
                if (v->radiusChunks > 0) {
                    constexpr i32 kMaxRadiusChunks = 24;
                    const i32 r = v->radiusChunks > kMaxRadiusChunks ? kMaxRadiusChunks : v->radiusChunks;
                    if (r != v->radiusChunks)
                        AVER_WARN("[ChunkWorld] PCGVOLUME '{}' asks for radius {}; clamped to {}",
                                  v->name, v->radiusChunks, r);
                    cwSettings.stream.loadRadius  = r;
                    cwSettings.stream.evictRadius = r + 2;
                }
            }

            // Scatter follows the terrain when a section is resident. The generator asks only "what
            // is the surface Z at (x, y)" and knows nothing about landscapes -- Aver.World does not
            // depend on Aver.Landscape, so the editor, which depends on both, closes this lambda.
            // The authored section wins where it exists; past its rim this falls through to the same
            // continuous noise the ring tiles' geometry is built from.
#if AVER_MODULE_LANDSCAPE
            if (landscapeLoaded_) {
                cwSettings.generator.heightSource = [this](f32 x, f32 y, f32& outZ) {
                    if (landscape::surfaceHeightAt(landscapeData_, x, y, outZ)) return true;
                    outZ = landscape::terrainHeightAt(x, y, landscapeNoiseParams_);
                    return true;
                };
            }
#endif

            world::RestoreOptions& restore = cw->streamer().restoreOptions();
#if AVER_MODULE_PBR
            restore.bindMaterial = [this](i32 token, const std::string& surface) {
                const pbr::MaterialHandle h = materialForSurface(surface);
                if (h) surfaceMaterials_[token] = h;
            };
#endif
#if AVER_MODULE_PHYSICS
            restore.createBody = [](scene::Entity e, const Vec3& worldPos, const Vec3& halfExtentCm) -> i32 {
                if (!aver_phys_ready()) return -1;
                const i32 body = aver_phys_add_static_box(worldPos.x, worldPos.y, worldPos.z,
                                                          halfExtentCm.x, halfExtentCm.y, halfExtentCm.z);
                if (body) aver_phys_set_entity(body, static_cast<i32>(e));
                return body;
            };
#endif

            std::string why;
            if (!cw->open(cwSettings, &why)) {
                // One field failing must not take the others down with it -- a level with a good
                // floor and a broken canopy should still show its floor.
                AVER_WARN("[ChunkWorld] field '{}' failed to open: {}",
                          v ? v->name : std::string("<none>"), why);
                continue;
            }

            AVER_INFO("[ChunkWorld] field '{}' -- worldDir='{}' loadRadius={} evictRadius={} "
                      "palette={} species threshold={:.2f} samples={}/axis (populated to {:.0f}m)",
                      v ? v->name : std::string("<none>"), cwSettings.worldDir,
                      cwSettings.stream.loadRadius, cwSettings.stream.evictRadius,
                      cwSettings.generator.palette.size(), cwSettings.generator.threshold,
                      cwSettings.generator.samplesPerAxis,
                      static_cast<f32>(cwSettings.stream.loadRadius * cwSettings.stream.chunkSizeCm) / 100.0f);
            built.push_back(std::move(cw));
        }

        if (built.empty()) {
            AVER_WARN("[ChunkWorld] cannot enable streaming: no density field produced a world");
            return;
        }

        chunkWorld_ = std::move(built[0]);
        chunkWorldsExtra_.clear();
        for (usize k = 1; k < built.size(); ++k) chunkWorldsExtra_.push_back(std::move(built[k]));

        chunkStreamHaveLastPos_ = false;
        chunkStreamLogsLeft_ = 8;
        chunkStreamStats_ = world::StreamStats{};
        AVER_INFO("[ChunkWorld] streaming enabled -- {} density field(s), chunkSize={}cm "
                  "verticalRadius={}",
                  built.size(), chunkWorld_->settings().stream.chunkSizeCm,
                  chunkWorld_->settings().stream.verticalRadius);
        warnIfCameraOutsideGeneratedBand();
    }

    // Says so when streaming is switched on somewhere nothing will ever load.
    //
    // The generator fills a SINGLE BAND of chunk layers -- surfaceChunkZ plus or minus
    // verticalRadius -- and the streamer never asks outside it. A camera above or below that band
    // gets a wanted-set that is empty by construction: no error, no warning, no chunks, and a panel
    // reading zero that looks identical to "still starting up".
    //
    // This is not hypothetical. Enabling streaming on the ElectricDreams sample does exactly that:
    // its 30000x30000 ground plane dominates frameCameraOn's bounding box, so the level-load camera
    // parks near Z=37000 while the default band reaches about -1600..+3200. Nothing loads and
    // nothing says why. The interaction predates streaming having a consumer at all; the silence is
    // the part that is fixable here, so fix the silence.
    //
    // A WARNING, NOT A CORRECTION. Moving the camera would be worse: a designer who switched this on
    // to look at their own authored level would be yanked somewhere else, and "the tool teleported
    // me" is a harder bug to understand than "the tool told me I was out of range".
    void warnIfCameraOutsideGeneratedBand() const {
        if (!chunkWorld_) return;
        const world::StreamSettings& st = chunkWorld_->settings().stream;
        if (st.chunkSizeCm <= 0) return;

        const i32 camChunkZ = world::floorDiv(static_cast<i32>(camPos_.z), st.chunkSizeCm);
        const i32 surfaceZ  = chunkWorld_->settings().generator.surfaceChunkZ;
        if (std::abs(camChunkZ - surfaceZ) <= st.verticalRadius) return;

        // Inclusive of the top layer's full height, so the number quoted is the last Z that can
        // actually contain something rather than the coordinate its floor sits at.
        const i64 lo = i64(surfaceZ - st.verticalRadius) * st.chunkSizeCm;
        const i64 hi = i64(surfaceZ + st.verticalRadius + 1) * st.chunkSizeCm;
        AVER_WARN("[ChunkWorld] the camera is at Z={:.0f}cm (chunk layer {}), outside the generated "
                  "band {}..{}cm (layers {}..{}). Nothing will load until it is inside that band -- "
                  "press F to focus something near ground level, or fly down.",
                  camPos_.z, camChunkZ, lo, hi, surfaceZ - st.verticalRadius, surfaceZ + st.verticalRadius);
    }

    // Spawns (or despawns) the graph-driven drone. OPT-IN, same shape as setChunkStreamingEnabled:
    // Window > Drone or --drone, nothing touched until asked for.
    //
    // TRANSIENT, LIKE A CHUNK-STREAMED ENTITY, ON PURPOSE. droneEntity_ is never pushed to
    // levelEntities_, so saveLevel/undo/redo never see it (they only ever walk
    // levelEntities_/undoStack_ -- see the ChunkWorld comment above this block for the identical
    // argument), and buildPanels' World Outliner filters it out explicitly by entity id, the same way
    // it filters chunkWorld_->owns(e).
    void setDroneEnabled(bool on) {
        if (on == (droneEntity_ != scene::kInvalidEntity)) return;

        if (!on) {
            scene::World& world = scene::World::instance();
            if (world.valid(droneEntity_)) { world.destroy(droneEntity_); world.flush(); }
#if AVER_MODULE_SCRIPTING
            if (scripts_.ready()) scripts_.graphUnload(static_cast<i32>(droneEntity_));
#endif
            AVER_INFO("[Drone] disabled -- entity #{} released", (u32)droneEntity_);
            droneEntity_ = scene::kInvalidEntity;
            droneGraphLoaded_ = false;
            droneHaveLastPos_ = false;
            return;
        }

        if (!project_.valid()) {
            AVER_WARN("[Drone] cannot enable: no project is open");
            return;
        }
#if AVER_MODULE_SCRIPTING
        if (!scripts_.ready()) {
            AVER_WARN("[Drone] cannot enable: the scripting host is not running ({})",
                      scripts_.declineReason());
            return;
        }
        if (!scripts_.graphAvailable()) {
            AVER_WARN("[Drone] cannot enable: this build's staged bridge exports no Graph entry "
                      "points -- rebuild with the .NET SDK present so Aver.Scripting.Bridge picks up "
                      "GraphLoad/GraphTick/GraphUnload");
            return;
        }
#else
        AVER_WARN("[Drone] cannot enable: this build has no scripting module (AVER_MODULE_SCRIPTING=OFF)");
        return;
#endif
        scene::World& world = scene::World::instance();
        Transform xf;
        // AT THE PLAYER START WHEN THE LEVEL HAS ONE. The drone is what flies when a project
        // declares no GameMode, so it IS the player for that session, and "where the player spawns
        // in" is exactly what a Player Start says. Falling back to the camera keeps every level that
        // has not placed one behaving as before.
        {
            Vec3 sp{}; f32 sy = 0.0f;
            if (playerStartTransform(sp, sy)) {
                xf.position = sp;
                xf.rotation = quatFromEulerDeg(Vec3{0.0f, 0.0f, sy});
                AVER_INFO("[Drone] spawning at the level's Player Start ({:.0f}, {:.0f}, {:.0f})",
                          sp.x, sp.y, sp.z);
            } else {
                xf.position = camPos_ + camForward() * kAddDistance;
                xf.rotation = Quat{0, 0, 0, 1};
            }
        }
        xf.scale = Vec3{kEditorCubeHalf, kEditorCubeHalf, kEditorCubeHalf};

        // FROZEN, same as spawnCube: the entity name is the asset path the mesh resolver hashes.
        static const std::string kDroneAsset = "Meshes/cube.ocmesh";
        const scene::Entity e = world.create(kDroneAsset, scene::kInvalidEntity, xf);
        if (e == scene::kInvalidEntity) { AVER_WARN("[Drone] the world refused a new entity"); return; }
        if (auto* mr = static_cast<scene::CMeshRenderer*>(
                world.addComponent(e, scene::kComponentMeshRenderer))) {
            mr->mesh = fnv1a64(std::string_view(kDroneAsset));
            mr->flags |= scene::kMeshRendererVisible;
            mr->aabbMin[0] = mr->aabbMin[1] = mr->aabbMin[2] = -1.0f;
            mr->aabbMax[0] = mr->aabbMax[1] = mr->aabbMax[2] =  1.0f;
        }
        // Deliberately NOT levelEntities_.push_back(e) and NOT pushEdit(...): see the comment above
        // this function for why.

#if AVER_MODULE_SCRIPTING
        // The graph comes from the PROJECT, named by --drone-graph, and the engine has no opinion
        // about what it is called.
        //
        // It used to be hardcoded as Content\Scripts\Drone.ocgraph, which made Sandbox.exe assume
        // every project contains a file of that name -- one sample project's content compiled into
        // the editor. A graph-driven actor is an engine feature; WHICH graph drives it is content,
        // the same way the editor does not know what any project's meshes are called.
        const std::string graphPath =
            droneGraphRel_.empty() ? std::string()
                                   : project_.contentDir() + "\\" + droneGraphRel_;
        if (graphPath.empty()) {
            AVER_WARN("[Drone] entity #{} spawned with NO graph: pass --drone-graph <path relative "
                      "to Content>, e.g. --drone-graph Scripts\\MyActor.ocgraph. It will sit still.",
                      (u32)e);
            droneGraphLoaded_ = false;
        } else {
            droneGraphLoaded_ = scripts_.graphLoad(static_cast<i32>(e), graphPath);
            if (!droneGraphLoaded_)
                AVER_WARN("[Drone] entity #{} spawned but its graph would not load from '{}' -- see "
                          "the [Graph] error line just above for why", (u32)e, graphPath);
        }
#endif
        droneEntity_ = e;
        droneTimeSeconds_ = 0.0f;
        droneHaveLastPos_ = false;
        droneLogsLeft_ = 30;
        AVER_INFO("[Drone] enabled -- entity #{} spawned at ({:.0f},{:.0f},{:.0f}), graph {}",
                  (u32)e, xf.position.x, xf.position.y, xf.position.z,
                  droneGraphLoaded_ ? "loaded" : "NOT loaded");
    }

    // Sum of triangle counts over every entity chunkWorld_ currently owns. O(residentEntities),
    // walked fresh each call rather than kept running -- residentEntities is a few hundred at most
    // and this only runs while the streaming panel is open or a log line needs it, not every frame
    // unconditionally.
    u64 residentTriangleCount() const {
        u64 total = 0;
        if (!chunkWorld_) return total;
        const scene::World& world = scene::World::instance();
        // Every field, not just the primary: a canopy streaming at radius 10 is exactly the triangles
        // someone reading this number is trying to account for.
        const auto add = [&](const world::ChunkWorld& cw) {
            for (const scene::Entity e : cw.streamedEntities()) {
                const auto* mr = world.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
                if (!mr) continue;
                const auto it = meshTris_.find(mr->mesh);
                if (it != meshTris_.end()) total += it->second;
            }
        };
        add(*chunkWorld_);
        for (const auto& extra : chunkWorldsExtra_) if (extra) add(*extra);
        return total;
    }

    // A small always-on-while-streaming readout of StreamStats. pendingLoads and failedLoads are
    // singled out because they are the two numbers that tell "working" (pendingLoads draining, zero
    // failures) from "not keeping up" (pendingLoads staying high) or "broken" (failedLoads growing).
#if AVER_WITH_IMGUI
// A pure-ImGui debug window. Guarded because uiActive() is a RUNTIME test and cannot make the
// ImGui:: names exist for the compiler -- see the mouse-capture block in onUpdate for the same trap.
    void buildChunkStreamingPanel() {
        if (!chunkWorld_) return;
        const world::StreamStats& s = chunkStreamStats_;
        ImGui::SetNextWindowPos(ImVec2(12.0f * dpi_, 60.0f * dpi_), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.85f);
        if (!ImGui::Begin("Chunk Streaming", nullptr, ImGuiWindowFlags_NoFocusOnAppearing)) {
            ImGui::End();
            return;
        }
        ImGui::Text("resident:  %u chunks / %u entities", s.residentChunks, s.residentEntities);
        ImGui::Text("this frame: +%u loaded, -%u evicted", s.loadedThisUpdate, s.evictedThisUpdate);
        ImGui::Text("in flight, %u/%u", s.pendingLoads, s.failedLoads);
        ImGui::SameLine();
        ImGui::TextDisabled("(pending / failed)");
        if (s.failedLoads > 0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1.0f), "!");
        }
        ImGui::Text("total loads: %u   avg load: %.2f ms", s.totalLoads,
                    s.totalLoads > 0 ? s.totalLoadMs / static_cast<f64>(s.totalLoads) : 0.0);
        ImGui::Text("last load: %.2f ms", s.lastLoadMs);
        ImGui::Text("resident triangles: %llu", static_cast<unsigned long long>(residentTriangleCount()));
        ImGui::Separator();
        ImGui::TextDisabled("%s", chunkWorld_->settings().worldDir.c_str());
        ImGui::End();
    }
#endif  // AVER_WITH_IMGUI

    // Loads an .ocworld into the world as ordinary scene entities: transform, mesh and name.
    //
    // TAKES Engine& (it did not before) so it can hand a real device down to loadLandscapeForLevel --
    // the landscape's GPU mesh cache is created lazily by onRender()'s own draw() call regardless, but
    // unloadLevel/unloadLandscape need a device to free what the PREVIOUS level left resident, and the
    // only device this function ever has is the one its own two callers already hold (applyProject and
    // buildUI's "Open Level"/"New Level" menu items).
    void loadLevel(Engine& eng, const std::string& path) {
        unloadLevel(eng);
        fmt::OcWorldData w;
        std::string why;
        if (!fmt::loadOcworld(path, w, &why)) { AVER_WARN("[Level] {}", why); return; }

        // CARRIED, NOT UNDERSTOOD. The editor has no UI for a PCGVOLUME and does not need one, but
        // saveLevel builds a fresh OcWorldData from the editor's own state -- so anything the editor
        // does not hold is GONE on the next save. That silently deleted every PCGVOLUME in the
        // level, including the one a new project is scaffolded with and the one the forest
        // generator writes: open, save, and the sky's field record no longer exists.
        //
        // The same reasoning as the project manifest keeping unknown keys: a tool that rewrites a
        // file it only partly understands must preserve the rest verbatim.
        levelPcgVolumes_ = w.pcgVolumes;
        // ...and the rest of the header, for exactly the same reason. Placements and PCG volumes are
        // stripped because they are already held elsewhere; what is left is identity, BUILD, ALGO,
        // SPAWN and the environment numbers the editor does not expose.
        levelHeader_ = w;
        levelHeader_.placements.clear();
        levelHeader_.pcgVolumes.clear();
        if (!levelPcgVolumes_.empty())
            AVER_INFO("[Level] carrying {} PCGVOLUME record(s) through the editor unchanged",
                      levelPcgVolumes_.size());

        // AND NOW THE SKY FIELD ACTUALLY REACHES THE CLOUD LAYER, as it already did in the packaged
        // runtime (GameApp::applySky). Carrying the record through a save was all the editor ever did
        // with it, so a project scaffolded with `PCGVOLUME name Sky` -- which is every new project --
        // opened onto a bare gradient, and the only ways to see the clouds the level had asked for
        // were the --clouds switch or ticking the box in the Sky panel by hand. An editor that draws
        // something other than what the level says is the one thing it must not do.
        //
        // BY NAME, not "the first field": a level may also declare a cave mask or a moisture field,
        // and quietly sampling one of those as the sky would read as a rendering bug rather than the
        // lookup mistake it would be.
        //
        // The floor is INVERTED into coverage. A density floor is the threshold below which the field
        // is empty, so a HIGH floor leaves LESS material standing -- less cloud, not more. Passing it
        // through unchanged would clear the sky exactly when the author asked for overcast.
        for (const fmt::OcPcgVolume& v : levelPcgVolumes_) {
            if (v.name != "Sky") continue;
            const f64 floorV = v.coverageFloor < 0.0 ? 0.0 : (v.coverageFloor > 1.0 ? 1.0 : v.coverageFloor);
            sky_.cloudsEnabled = true;
            sky_.cloudSeed     = v.seed;
            sky_.cloudCoverage = static_cast<f32>(1.0 - floorV);
            AVER_INFO("[Level] sky field '{}' drives the cloud layer: seed {}, coverage {:.2f}",
                      v.name, sky_.cloudSeed, sky_.cloudCoverage);
            break;
        }

        // The placement loop is aver::world::instantiate now, shared with the game runtime. What is
        // left here is the part that is genuinely the EDITOR's: its label table, its entity->body
        // map, and the per-entity record saveLevel needs to write the level back out unchanged.
        // TERRAIN FIRST, THEN THE THINGS THAT STAND ON IT. This used to run at the very end of
        // loadLevel, after every placement had already been instantiated, which was harmless only
        // while the ground was a flat plane at z=0. A `snap` placement asks the ground how high it
        // is, so the ground has to exist by then.
#if AVER_MODULE_LANDSCAPE
        loadLandscapeForLevel(eng.device(), path, w);
#endif

        world::InstantiateOptions opt;
#if AVER_MODULE_LANDSCAPE
        // The same surface the scatter follows, so a hand-placed tree and a scattered fern sitting
        // a metre apart agree about where the ground is.
        opt.groundHeightAt = [this](f64 x, f64 y, f64& outZ) {
            if (!landscapeLoaded_) return false;
            f32 z = 0.0f;
            if (!landscape::surfaceHeightAt(landscapeData_, static_cast<f32>(x),
                                            static_cast<f32>(y), z)) return false;
            outZ = static_cast<f64>(z);
            return true;
        };
#endif
#if AVER_MODULE_PBR
        opt.bindMaterial = [this](i32 token, const std::string& surface) {
            const pbr::MaterialHandle h = materialForSurface(surface);
            if (h) surfaceMaterials_[token] = h;
        };
#endif
        const world::LevelInstance inst = world::instantiate(w, opt);
        levelEntities_ = inst.entities;
#if AVER_MODULE_PHYSICS
        levelBodies_ = inst.bodies;
#endif
        for (usize k = 0; k < inst.entities.size(); ++k) {
            const scene::Entity e = inst.entities[k];
            const fmt::OcWorldPlacement& p = w.placements[inst.placementIndex[k]];
            entityLabels_[static_cast<u32>(e)] = makeEntityLabel(p.material, p.asset);
            // WHY `collide` IS REMEMBERED AND THE MATERIAL IS NOT. The surface survives on the
            // entity -- CMeshRenderer::material -- and saveLevel reads it back through
            // aver_scene_material_name. `nocollide` has no component at all: it is a load-time
            // instruction and nothing on the entity records it afterwards. Inferring it from
            // entityBodies_ would be wrong, because a level opened before aver_phys_init has no
            // bodies for ANY placement and would save as though every one of them said nocollide.
            entityCollide_[static_cast<u32>(e)] = p.collide;
#if AVER_MODULE_PHYSICS
            if (inst.entityBody[k] >= 0) entityBodies_[static_cast<u32>(e)] = inst.entityBody[k];
#endif
        }

#if AVER_MODULE_FRAMEWORK
        // GRAPH-AS-CLASS / any other class placement -- collected here, SPAWNED LATER by
        // spawnClassPlacements(), for the identical ordering reason GameLevel.hpp's classPlacements_
        // documents: applyProject's own "Loading level" stage (this function) runs BEFORE its
        // "Starting scripts" stage, so a class declared from a .ocgraph is not registered yet at this
        // point -- aver_fw_class_find would always miss it if called from inside loadLevel itself.
        classPlacements_.clear();
        for (const fmt::OcWorldPlacement& p : w.placements)
            if (!p.className.empty()) classPlacements_.push_back(p);
#endif

        // A level that states where the player starts gets a visible, movable marker for it. Without
        // this the SPAWN record was invisible in the editor: authored only by hand-editing the file,
        // and impossible to see or move once written.
#if AVER_MODULE_SCENE
        playerStart_ = scene::kInvalidEntity;
        if (w.hasSpawn) {
            playerStart_ = makePlayerStart(Vec3{static_cast<f32>(w.spawnX), static_cast<f32>(w.spawnY),
                                                 static_cast<f32>(w.spawnZ)},
                                            static_cast<f32>(w.spawnYaw));
        }
#endif

        if (w.hasFog) {
            levelFog_ = static_cast<f32>(w.fogDensity);
            fogColor_[0] = static_cast<f32>(w.fogColor[0]);
            fogColor_[1] = static_cast<f32>(w.fogColor[1]);
            fogColor_[2] = static_cast<f32>(w.fogColor[2]);
            hasLevelFog_ = true;
        }
        applyLevelSky(w);
        levelPath_ = path;
        levelName_ = w.name;

        sel_ = -1;
        selEntity_ = scene::kInvalidEntity;

        if (!w.placements.empty()) frameCameraOn(w);

        AVER_INFO("[Level] '{}' loaded from {} ({} placement(s))", w.name, path, w.placements.size());

    }

#if AVER_MODULE_FRAMEWORK
    // GRAPH-AS-CLASS / any other class placement: mirrors GameLevel::spawnClassPlacements (the game
    // runtime's own copy of this same pass) -- see that method's own comment for the full "why a
    // separate, later call rather than inline in loadLevel" story. Spawned FOR REAL
    // (aver_fw_spawn), not previewed: there is no existing "placed-in-level class instance, live in
    // edit mode, promoted at Play" precedent anywhere in this tree (aver_fw_spawn_preview's only
    // caller is the single-instance Actor Editor) to build on instead, and this is consistent with
    // how every ORDINARY mesh placement already behaves in this editor -- live immediately on load,
    // with no separate "inert until Play" state. The accepted consequence, same as GameLevel's own
    // copy: browsing a level with a placed class in it runs that class's OnTick immediately, even
    // outside Play.
    void spawnClassPlacements() {
        for (const fmt::OcWorldPlacement& p : classPlacements_) {
            const int32_t c = aver_fw_class_find(p.className.c_str());
            if (c == 0) {
                AVER_WARN("[Level] placement names class '{}', which is not declared -- skipped", p.className);
                continue;
            }

            f64 pz = p.z;
#if AVER_MODULE_LANDSCAPE
            // Same ground query loadLevel's own opt.groundHeightAt uses -- not persisted from there
            // (opt is local to loadLevel), so re-expressed here rather than threaded through as a
            // member for one caller.
            if (p.snapToGround && landscapeLoaded_) {
                f32 gz = 0.0f;
                if (landscape::surfaceHeightAt(landscapeData_, static_cast<f32>(p.x),
                                               static_cast<f32>(p.y), gz))
                    pz = static_cast<f64>(gz) + p.z;
            }
#endif
            const f32 pos3[3]  = {static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(pz)};
            const Quat rot     = world::quatFromEulerDeg(Vec3{static_cast<f32>(p.roll),
                                                               static_cast<f32>(p.pitch),
                                                               static_cast<f32>(p.yaw)});
            const f32 quat4[4]  = {rot.x, rot.y, rot.z, rot.w};
            const f32 scale3[3] = {static_cast<f32>(p.sx), static_cast<f32>(p.sy), static_cast<f32>(p.sz)};

            const int32_t e = aver_fw_spawn(c, p.className.c_str(), pos3, quat4, scale3);
            if (e == 0) {
                AVER_WARN("[Level] class '{}' failed to spawn at ({:.0f}, {:.0f}, {:.0f})",
                          p.className, pos3[0], pos3[1], pos3[2]);
                continue;
            }
            levelClassInstances_.push_back(e);
        }
        if (!levelClassInstances_.empty())
            AVER_INFO("[Level] {} class instance(s) placed -- an entity exists for each; whether its graph "
                      "COMPILED is reported per instance above, because aver_fw_spawn returns a live entity "
                      "even when the managed bind behind it failed, so this count is placement, not success",
                      levelClassInstances_.size());
    }
#endif

    // Applies a level's SUN and SKY records to the live atmosphere.
    //
    // The SUN half is NEW BEHAVIOUR: OcWorld has always parsed sunDir/sunColor/sunLux and nothing
    // has ever read them, so every level in existence has been lit by the editor's default sun
    // while its own record sat there looking authoritative.
    void applyLevelSky(const fmt::OcWorldData& w) {
        if (w.hasSun) {
            for (int i = 0; i < 3; ++i) {
                sky_.sunDirection[i] = static_cast<f32>(w.sunDir[i]);
                sunColor_[i] = static_cast<f32>(w.sunColor[i]);
            }
            // `lux` WAS STILL BEING DROPPED after the comment above said sunDir/sunColor no longer
            // were. Two of the three got wired; the brightness did not, so a level could state any
            // sun intensity it liked and every scene rendered at the editor's default 3.0. It is
            // silent, and it is invisible in a diff -- the value parses, round-trips through a save,
            // and never reaches a pixel. Caught by changing lux 112000 -> 34000 and getting a
            // BIT-IDENTICAL frame back.
            //
            // The divisor makes the two defaults agree rather than inventing a constant: OcWorldData
            // defaults sunLux to 100000, rhi::SkyAtmosphere defaults sunIntensity to 3.0, so a level
            // that states neither, and a level that states exactly the format default, both land on
            // the value the engine already used. Nothing that omits SUN changes brightness.
            sky_.sunIntensity = static_cast<f32>(w.sunLux / (100000.0 / 3.0));
            // A sun below the horizon is legal -- the physical model renders night -- but under the
            // authored dome it silently lit everything from underneath, so no level was ever told.
            // Say it out loud rather than clamping: only the author knows if they meant it.
            f32 elev = 0.0f, azim = 0.0f;
            sky_.sunAngles(elev, azim);
            if (elev < 0.0f)
                AVER_WARN("[Level] SUN is {:.1f} degrees BELOW the horizon (dir {:.3f} {:.3f} {:.3f}). "
                          "The physical sky renders that as night; --sky-authored lights from below "
                          "as it always did.", elev, w.sunDir[0], w.sunDir[1], w.sunDir[2]);
            hasLevelSun_ = true;
        }
        if (w.hasSky) {
            sky_.model = w.skyPhysical ? rhi::SkyModel::Physical : rhi::SkyModel::Authored;
            if (w.skyMieScatter   >= 0.0) sky_.air.mieScatter       = static_cast<f32>(w.skyMieScatter);
            if (w.skyMultiScatter >= 0.0) sky_.air.multiScatterGain = static_cast<f32>(w.skyMultiScatter);
            if (w.skyViewSteps    >  0)   sky_.air.viewSteps        = w.skyViewSteps;
            if (w.skyAerialSteps  >  0)   sky_.air.aerialSteps      = w.skyAerialSteps;
            hasLevelSky_ = true;
        }
    }

    // Puts the editor camera where the whole level is visible, and fits the fly speed and the GI
    // volume to its bounds.
    void frameCameraOn(const fmt::OcWorldData& w) {
        // THE MESH BOX, SCALED AND ROTATED -- not p.sx/sy/sz used as if it were a size. This loop
        // read fabs(p.sx/sy/sz) directly as a half-extent in centimetres, and sx/sy/sz is a
        // dimensionless SCALE MULTIPLIER: modules/world/src/LevelInstance.cpp assigns it straight to
        // Transform::scale and nothing converts it to a size. An ordinary PLACE leaves it at 1.0, so
        // every prop in the level contributed a 1cm cube and this "bounds" was really the point
        // cloud of placement POSITIONS -- which framed the camera on where things were dropped
        // rather than on how big they are, and (since giExtent_ is set from the same radius below)
        // handed Voxi a volume sized the same way.
        //
        // It looked right because of one coincidence: PLACEG is conventionally used with a unit-cube
        // mesh for a ground slab, and for a mesh spanning +/-1 a scale and a half-extent are the
        // same number. That is a property of that mesh, not of the format.
        //
        // Kept identical to modules/runtime.game/src/GameLevel.cpp's placementBounds, deliberately:
        // the editor's preview and the shipped game must fit the same volume to the same level, and
        // these two loops have already drifted once.
        static const Vec3 kCorner[8] = {{-1,-1,-1},{1,-1,-1},{-1,1,-1},{1,1,-1},
                                        {-1,-1, 1},{1,-1, 1},{-1,1, 1},{1,1, 1}};
        Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        for (const fmt::OcWorldPlacement& p : w.placements) {
            const Vec3 c{static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)};
            // An asset with no loaded bounds contributes its position only: it occupies no space we
            // can prove, and inventing one would let a single bad line inflate the whole volume.
            Vec3 mlo{0,0,0}, mhi{0,0,0};
            const auto itB = meshBounds_.find(fnv1a64(std::string_view(p.asset)));
            if (itB != meshBounds_.end()) { mlo = itB->second.first; mhi = itB->second.second; }
            const Vec3 mc{(mlo.x+mhi.x)*0.5f, (mlo.y+mhi.y)*0.5f, (mlo.z+mhi.z)*0.5f};
            const Vec3 mh{(mhi.x-mlo.x)*0.5f, (mhi.y-mlo.y)*0.5f, (mhi.z-mlo.z)*0.5f};
            const Quat rot = world::quatFromEulerDeg(Vec3{static_cast<f32>(p.roll),
                                                          static_cast<f32>(p.pitch),
                                                          static_cast<f32>(p.yaw)});
            for (const Vec3& k : kCorner) {
                const Vec3 local{(mc.x + k.x*mh.x) * static_cast<f32>(p.sx),
                                 (mc.y + k.y*mh.y) * static_cast<f32>(p.sy),
                                 (mc.z + k.z*mh.z) * static_cast<f32>(p.sz)};
                const Vec3 wpt = c + rot.rotate(local);
                lo.x = std::fmin(lo.x, wpt.x); hi.x = std::fmax(hi.x, wpt.x);
                lo.y = std::fmin(lo.y, wpt.y); hi.y = std::fmax(hi.y, wpt.y);
                lo.z = std::fmin(lo.z, wpt.z); hi.z = std::fmax(hi.z, wpt.z);
            }
        }
        // Every placement missing and the sentinels never moved: a level with no placements at all.
        // Guarded because the radius below would otherwise be computed from 1e9-(-1e9).
        if (w.placements.empty() || lo.x > hi.x) { lo = Vec3{0,0,0}; hi = Vec3{0,0,0}; }
        const Vec3 centre{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
        const f32 radius = std::fmax(1.0f, 0.5f * std::sqrt((hi.x-lo.x)*(hi.x-lo.x) +
                                                            (hi.y-lo.y)*(hi.y-lo.y) +
                                                            (hi.z-lo.z)*(hi.z-lo.z)));
        const f32 dist = radius * 1.6f;
        camPos_ = Vec3{centre.x - dist * 0.65f, centre.y - dist * 0.65f, centre.z + dist * 0.55f};
        const Vec3 look = (centre - camPos_).getSafeNormal();
        yaw_   = std::atan2(look.y, look.x);
        pitch_ = std::asin(std::fmax(-1.0f, std::fmin(1.0f, look.z)));
        flySpeed_ = std::fmax(flySpeed_, radius * 0.02f);
        // This is a teleport, not a move: the next chunk-streaming update must not see this as a
        // (huge, one-frame) velocity computed against wherever the camera used to be.
        chunkStreamHaveLastPos_ = false;

#if AVER_MODULE_VOXI
        giCenter_ = centre;
        giExtent_ = radius;
#endif
    }

    // Loads the project's start map, resolved against its content directory. Missing is not an error.
    void loadStartMap(Engine& eng) {
        if (!project_.valid() || project_.startMap.empty()) return;
        const std::string path = project_.contentDir() + "\\" + project_.startMap;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            AVER_INFO("[Level] start map '{}' does not exist yet - the world starts empty", project_.startMap);
            levelName_ = std::filesystem::path(project_.startMap).stem().string();
            levelPath_ = path;
            return;
        }
        loadLevel(eng, path);
    }

    // Destroys the loaded level's entities and everything keyed to them: labels, bodies, undo.
    void unloadLevel(Engine& eng) {
        (void)eng;   // only read under AVER_MODULE_LANDSCAPE, at the end of this function
#if AVER_MODULE_FRAMEWORK
        // BEFORE the raw-entity loop below, and through aver_fw_destroy rather than world.destroy() --
        // see GameLevel::unload's identical comment for why (the managed-dispatch unbind hook is what
        // releases a graph-class instance's GraphHost/VAR storage).
        for (const int32_t e : levelClassInstances_) aver_fw_destroy(e);
        levelClassInstances_.clear();
        classPlacements_.clear();
#endif
        scene::World& world = scene::World::instance();
        for (const scene::Entity e : levelEntities_) if (world.valid(e)) world.destroy(e);
        levelEntities_.clear();
        entityLabels_.clear();
        labelCounts_.clear();
        entityBodies_.clear();
        // Cleared with the rest of the level's state: carrying one level's PCG records into the
        // next would write them into a file that never had them.
        levelPcgVolumes_.clear();
        levelHeader_ = fmt::OcWorldData{};
        entityCollide_.clear();
        undoStack_.clear();
        redoStack_.clear();
        editToEntity_.clear();
        entityToEdit_.clear();
        editBeforeValid_ = false;
        if (sel_ == kSelScene) { sel_ = -1; selEntity_ = scene::kInvalidEntity; }
#if AVER_MODULE_PHYSICS
        for (const int32_t b : levelBodies_) aver_phys_remove_body(b);
        levelBodies_.clear();
#endif
        hasLevelFog_ = false;
        hasLevelSun_ = false;
        hasLevelSky_ = false;
        levelPath_.clear();
#if AVER_MODULE_LANDSCAPE
        unloadLandscape(eng.device());
#endif
    }

    // Writes the level's own entities back out to an .ocworld. Spawned actors are not written.
    bool saveLevel(const std::string& path) {
        scene::World& world = scene::World::instance();
        // STARTS FROM WHAT THE FILE SAID, not from a default-constructed OcWorldData. Everything the
        // editor does not model -- ID, BUILD, ALGO, SPAWN, the sun's lux -- rides through untouched;
        // the lines below overwrite only what the editor genuinely owns. See levelHeader_.
        fmt::OcWorldData w = levelHeader_;
        w.name = levelName_.empty() ? std::string("untitled") : levelName_;
        // THE MARKER IS THE TRUTH WHEN THERE IS ONE. saveLevel's own banner used to list SPAWN among
        // the fields that "ride through untouched" -- correct while nothing could edit it, wrong now
        // that a Player Start can be placed and dragged. A level with no marker keeps whatever SPAWN
        // it arrived with, so opening and saving a hand-authored level still cannot lose it.
        {
            Vec3 sp{}; f32 sy = 0.0f;
            if (playerStartTransform(sp, sy)) {
                w.hasSpawn = true;
                w.spawnX = sp.x; w.spawnY = sp.y; w.spawnZ = sp.z; w.spawnYaw = sy;
            }
        }

        w.hasFog = hasLevelFog_;
        w.fogDensity = levelFog_;
        w.fogColor[0] = fogColor_[0]; w.fogColor[1] = fogColor_[1]; w.fogColor[2] = fogColor_[2];

        // The sun and the sky go back out whenever the level carried them, so an edit in the
        // Details panel survives a save rather than being silently dropped on the next load.
        w.hasSun = hasLevelSun_;
        w.hasSky = hasLevelSky_;
        for (int i = 0; i < 3; ++i) {
            w.sunDir[i] = sky_.sunDirection[i];
            w.sunColor[i] = sunColor_[i];
        }
        // Straight back out, in the order they were read. See loadLevel.
        w.pcgVolumes      = levelPcgVolumes_;
        w.skyPhysical     = sky_.model == rhi::SkyModel::Physical;
        w.skyMieScatter   = sky_.air.mieScatter;
        w.skyMultiScatter = sky_.air.multiScatterGain;
        w.skyViewSteps    = sky_.air.viewSteps;
        w.skyAerialSteps  = sky_.air.aerialSteps;

        for (const scene::Entity e : levelEntities_) {
            if (!world.valid(e)) continue;
            const auto* loc = world.component<scene::CLocal>(e, scene::kComponentLocal);
            const auto* mr  = world.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
            if (!loc) continue;
            fmt::OcWorldPlacement p;
            p.asset = world.name(e);
            p.x = loc->xf.position.x; p.y = loc->xf.position.y; p.z = loc->xf.position.z;
            const Vec3 euler = eulerDegFromQuat(loc->xf.rotation);
            p.roll = euler.x; p.pitch = euler.y; p.yaw = euler.z;
            p.sx = loc->xf.scale.x; p.sy = loc->xf.scale.y; p.sz = loc->xf.scale.z;
            // THE SURFACE, WHICH USED TO BE DROPPED ON EVERY SAVE. `(void)mr;` sat here and the
            // material line was simply missing, so opening a level and saving it stripped the
            // surface token off every placement in the file. The token itself must never be written
            // -- it is a process-local intern id (docs/CHUNKS.md 5.1) -- so it goes back out as the
            // NAME it was interned under.
            if (mr && mr->material) p.material = aver_scene_material_name(mr->material);
            // `nocollide` now round-trips. This was hardcoded true, so a placement authored
            // nocollide came back colliding and quietly gained a static body on the next load.
            // Entities created in the editor are absent from the map and keep the true default.
            const auto collideIt = entityCollide_.find(static_cast<u32>(e));
            p.collide = collideIt == entityCollide_.end() ? true : collideIt->second;
            w.placements.push_back(std::move(p));
        }

        // CLASS PLACEMENTS GO BACK OUT TOO, AND UNTIL NOW THEY DID NOT -- this loop rebuilds a
        // placement from each entity's SCENE COMPONENTS, and a `class=` placement has none of the
        // things it reads: no CMeshRenderer, no asset path, nothing that survives the round trip
        // through the world. They are parsed into classPlacements_ (see spawnClassPlacements, which
        // is the only other thing that touches it) and were then simply never written, so opening a
        // level and saving it DELETED every graph class in it.
        //
        // That is the whole of a graph-only project. Saving the FirstPerson template's map removed
        // its game mode and all three targets and left a floor and two crates -- a level that loads
        // fine, starts no play session, and has no player, with nothing in the log to say why. It is
        // the same defect as the material token four lines above ("used to be dropped on every
        // save"), one field further along, and it is worse because the thing dropped is not a
        // property of an object but the object itself.
        //
        // Written from classPlacements_ verbatim rather than rebuilt: the editor cannot currently
        // EDIT a class placement (there is no entity to select and drag), so the copy it read in is
        // still the truth, and passing it straight through is both correct and the only honest thing
        // to do until that changes.
        for (const fmt::OcWorldPlacement& cp : classPlacements_) w.placements.push_back(cp);

        std::string why;
        if (!fmt::saveOcworld(path, w, &why)) { AVER_WARN("[Level] save failed: {}", why); return false; }
        AVER_INFO("[Level] saved {} placement(s) to {}", w.placements.size(), path);
        return true;
    }

    std::vector<scene::Entity> levelEntities_;
    std::string levelPath_, levelName_;
    bool hasLevelSun_ = false;
    bool hasLevelSky_ = false;
    bool hasLevelFog_ = false;
    f32  levelFog_ = 0.0002f;

#if AVER_MODULE_FRAMEWORK
    // GRAPH-AS-CLASS / any other class placement -- see loadLevel's own comment (where
    // classPlacements_ is populated) and spawnClassPlacements' (where it is consumed and
    // levelClassInstances_ is filled) for the full ordering story. Mirrors GameLevel.hpp's own pair
    // of members, one for one.
    std::vector<fmt::OcWorldPlacement> classPlacements_;
    std::vector<int32_t> levelClassInstances_;
#endif

    // ---------------- chunk streaming (opt-in, Window > Chunk Streaming) ----------------
    // Owned only while streaming is switched on -- created by setChunkStreamingEnabled(true),
    // destroyed by setChunkStreamingEnabled(false). Deliberately disjoint from levelEntities_: the
    // World Outliner filters streamed entities out via chunkWorld_->owns(e) (buildPanels), and
    // saveLevel/undo/redo never see them because they only ever walk levelEntities_/undoStack_,
    // which streamed entities are never added to.
    std::unique_ptr<world::ChunkWorld> chunkWorld_;

    // The level's SECOND and further density fields, one ChunkWorld each. Empty for every level that
    // declares one PCGVOLUME, which is the shape every level had before this existed.
    //
    // ONE WORLD PER FIELD, rather than one generator holding several specs. A ChunkWorld carries its
    // own StreamSettings, and the streaming RADIUS is the whole point: one field forces one radius
    // for everything, and one radius cannot serve both a dense floor and a sparse canopy. At the
    // shipped radius of 3 the world is populated for 48 m while the camera sees to the horizon, so
    // the scatter ends at a visible ring -- raising that one radius multiplies the ground cover too,
    // which is invisible at that distance and is most of the triangles. Separate worlds let a canopy
    // stream at radius 10 with 3 samples/axis while the floor stays at 3 with 12.
    //
    // It is also the SMALLER change: extending ChunkGenerator to hold several specs would touch the
    // reproducibility-pinned sampling core its header is explicit about protecting.
    //
    // chunkWorld_ above stays the PRIMARY -- the first non-Sky volume -- so the thirty-odd sites that
    // only read settings for a panel or a log keep working unchanged. Only the five that must see
    // every world (update, owns, shutdown, streamedEntities, and the builder) walk both.
    std::vector<std::unique_ptr<world::ChunkWorld>> chunkWorldsExtra_;

    world::StreamStats chunkStreamStats_;
    Vec3 chunkStreamLastCamPos_{};
    bool chunkStreamHaveLastPos_ = false;   // false right after enabling or after a camera teleport,
                                             // so the next frame reports zero velocity instead of a
                                             // one-frame spike computed against a stale position.
    u32  chunkStreamLogsLeft_ = 8;          // first few load/evict frames get an explicit log line
#if AVER_MODULE_PHYSICS
    std::vector<int32_t> levelBodies_;
#endif

    // ---------------- graph-driven drone (opt-in, Window > Drone or --drone) ----------------
    // Proves a native scene can be driven by an .ocgraph end to end: spawned by setDroneEnabled(true),
    // ticked from onUpdate via scripts_.graphTick(), released by setDroneEnabled(false). Deliberately
    // disjoint from levelEntities_ for the exact reason chunkWorld_'s entities are (see the comment
    // above): transient, never saved, never undo-tracked. The World Outliner filters it out the same
    // way it filters chunkWorld_->owns(e) -- see buildPanels.
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
    bool mouseCaptured_ = false;
    i32  captureAnchorX_ = 0, captureAnchorY_ = 0;
    f32  captureDx_ = 0.0f, captureDy_ = 0.0f;
    Window* window_ = nullptr;   // borrowed from the engine in onInit, for the HWND

#if AVER_WITH_IMGUI
    editor::UiRegistry uiReg_;   // what the editor drew this frame, by name
#endif
#if AVER_MODULE_MCP
    mcp::McpBridge mcp_;         // inert until --mcp asks for it
    u16  mcpPort_ = 0;           // 0 = never asked for
#if AVER_MODULE_SCENE && AVER_MODULE_SCRIPTING
    // State for the "graph" ABI's load/attach/tick trio (see registerMcpAbis): load stages a path,
    // attach binds+compiles it onto a caller-chosen entity via scripts_.graphLoad, tick re-drives
    // that one entity's graph with a caller-supplied absolute time. One graph at a time, deliberately
    // -- this is a debugging/proving seam for an agent, not a general multi-entity graph manager.
    std::string  mcpGraphPath_;
    scene::Entity mcpGraphEntity_ = scene::kInvalidEntity;
    f32          mcpGraphTimeSeconds_ = 0.0f;
#endif

    // Applies one MCP command: an ABI call, or synthetic input posted to the window as Win32 messages.
    void applyMcpCommand(const mcp::Command& c) {
        HWND hwnd = window_ ? static_cast<HWND>(window_->nativeHandle()) : nullptr;
        if (!hwnd) return;

        if (c.name == "abi") {
            std::string result, why;
            if (mcp_.callAbi(c.abi, result, why))
                AVER_INFO("[Mcp] {}::{} -> {}", c.abi.module, c.abi.fn, result);
            else
                AVER_WARN("[Mcp] {}::{} refused: {}", c.abi.module, c.abi.fn, why);
            return;
        }

        for (const mcp::InputEvent& e : c.events) {
            const LPARAM lp = MAKELPARAM(e.x, e.y);
            switch (e.kind) {
                case mcp::InputEvent::Kind::MouseMove: {
                    POINT pt{ e.x, e.y };
                    ::ClientToScreen(hwnd, &pt);
                    ::SetCursorPos(pt.x, pt.y);
                    ::PostMessageW(hwnd, WM_MOUSEMOVE, 0, lp);
                    if (::GetForegroundWindow() != hwnd) ::SetForegroundWindow(hwnd);
                    break;
                }
                case mcp::InputEvent::Kind::MouseDown:
                    ::PostMessageW(hwnd,
                        e.button == 1 ? WM_RBUTTONDOWN : e.button == 2 ? WM_MBUTTONDOWN : WM_LBUTTONDOWN,
                        e.button == 1 ? MK_RBUTTON : e.button == 2 ? MK_MBUTTON : MK_LBUTTON, lp);
                    break;
                case mcp::InputEvent::Kind::MouseUp:
                    ::PostMessageW(hwnd,
                        e.button == 1 ? WM_RBUTTONUP : e.button == 2 ? WM_MBUTTONUP : WM_LBUTTONUP,
                        0, lp);
                    break;
                case mcp::InputEvent::Kind::KeyDown:
                    ::PostMessageW(hwnd, WM_KEYDOWN, static_cast<WPARAM>(e.key), 0);
                    break;
                case mcp::InputEvent::Kind::KeyUp:
                    ::PostMessageW(hwnd, WM_KEYUP, static_cast<WPARAM>(e.key), 0);
                    break;
                case mcp::InputEvent::Kind::Text:
                    // WM_CHAR per code unit: WM_KEYDOWN carries a virtual key, not a character.
                    for (char ch : e.text)
                        ::PostMessageW(hwnd, WM_CHAR, static_cast<WPARAM>(static_cast<unsigned char>(ch)), 0);
                    break;
            }
        }
    }

    // Registers every built module's plain-C seam with the MCP bridge, under its own name.
    void registerMcpAbis() {
        mcp_.registerAbi("editor", [this](const mcp::AbiCall& a, std::string& r, std::string& w) {
            if (a.fn == "tool") {
                // kToolNames[4..7] are the sculpt tools, present only where AVER_MODULE_LANDSCAPE is
                // -- the bound below must track its size or an in-range request would read past it.
#if AVER_MODULE_LANDSCAPE
                constexpr int kMaxTool = 7;
#else
                constexpr int kMaxTool = 3;
#endif
                if (a.args.empty()) { w = "tool needs a tool index 0-" + std::to_string(kMaxTool); return false; }
                const int t = static_cast<int>(a.args[0]);
                if (t < 0 || t > kMaxTool) { w = "tool index out of range 0-" + std::to_string(kMaxTool); return false; }
                tool_ = static_cast<Tool>(t);
                r = kToolNames[t];
                return true;
            }
            if (a.fn == "screenshot") {
                if (a.text.empty()) { w = "screenshot needs a path in \"text\""; return false; }
                shot_ = a.text;
                capDone_ = false;
                r = a.text;
                return true;
            }
            if (a.fn == "mouse") {
#if AVER_WITH_IMGUI
                const ImGuiIO& io = ImGui::GetIO();
                char b[160];
                std::snprintf(b, sizeof b, "imgui pos=(%.0f,%.0f) display=(%.0f,%.0f) down=%d focus=%d",
                              io.MousePos.x, io.MousePos.y, io.DisplaySize.x, io.DisplaySize.y,
                              io.MouseDown[0] ? 1 : 0, io.AppFocusLost ? 0 : 1);
                r = b;
                return true;
#else
                w = "this build has no UI";
                return false;
#endif
            }
            if (a.fn == "widgets") {
                r = uiReg_.describe();
                if (r.empty()) { w = "nothing tracked yet -- no UI frame has completed"; return false; }
                return true;
            }
            w = "editor has no entry point '" + a.fn + "'";
            return false;
        });
#if AVER_MODULE_PHYSICS
        mcp_.registerAbi("physics", [](const mcp::AbiCall& a, std::string& r, std::string& w) {
            if (a.fn == "bodyCount") { r = std::to_string(aver_phys_body_count()); return true; }
            if (a.fn == "ready")     { r = aver_phys_ready() ? "1" : "0"; return true; }
            w = "aver_phys_" + a.fn + " is not exposed";
            return false;
        });
#endif

        // ALWAYS REGISTERED, even when this build has no scene module: a client asking `modules`
        // sees "world" either way, and a call into it gets a clear "this build has no scene module"
        // refusal rather than the generic "no ABI registered" one -- see the house rule this task
        // came with: degrade the entry points, do not compile them out inconsistently.
        mcp_.registerAbi("world", [this](const mcp::AbiCall& a, std::string& r, std::string& w) {
#if AVER_MODULE_SCENE
            if (a.fn == "stream_on") {
                setChunkStreamingEnabled(true);
                if (!chunkWorld_) {
                    w = "streaming did not turn on -- see the editor log (no project open, or "
                        "ChunkWorld::open failed)";
                    return false;
                }
                r = "on";
                return true;
            }
            if (a.fn == "stream_off") {
                setChunkStreamingEnabled(false);
                r = "off";
                return true;
            }
            if (a.fn == "stream_stats") {
                const world::StreamStats& s = chunkStreamStats_;
                char buf[320];
                std::snprintf(buf, sizeof buf,
                    "streaming=%s residentChunks=%u residentEntities=%u loadedThisUpdate=%u "
                    "evictedThisUpdate=%u pendingLoads=%u failedLoads=%u totalLoads=%u "
                    "lastLoadMs=%.3f totalLoadMs=%.3f",
                    chunkWorld_ ? "on" : "off", s.residentChunks, s.residentEntities,
                    s.loadedThisUpdate, s.evictedThisUpdate, s.pendingLoads, s.failedLoads,
                    s.totalLoads, s.lastLoadMs, s.totalLoadMs);
                r = buf;
                return true;
            }
            if (a.fn == "warp") {
                if (a.args.size() < 3) { w = "warp needs 3 args: x y z (world centimetres)"; return false; }
                camPos_ = Vec3{static_cast<f32>(a.args[0]), static_cast<f32>(a.args[1]),
                               static_cast<f32>(a.args[2])};
                // A teleport, not a move: the next chunk-streaming update must not see this as a
                // huge one-frame velocity computed against wherever the camera used to be -- the
                // same rule frameCameraOn's own comment states for the same reason.
                chunkStreamHaveLastPos_ = false;
                char buf[96];
                std::snprintf(buf, sizeof buf, "camPos=(%.1f,%.1f,%.1f)", camPos_.x, camPos_.y, camPos_.z);
                r = buf;
                return true;
            }
            w = "world has no entry point '" + a.fn + "'";
            return false;
#else
            (void)a;
            w = "this build has no scene module (AVER_MODULE_SCENE=OFF) -- chunk streaming and the "
                "world ABI are unavailable";
            return false;
#endif
        });

        // ALWAYS REGISTERED too, for the same reason as "world" above.
        mcp_.registerAbi("graph", [this](const mcp::AbiCall& a, std::string& r, std::string& w) {
#if AVER_MODULE_SCENE
            if (a.fn == "entity_pos") {
                if (a.args.empty()) { w = "entity_pos needs an entity id"; return false; }
                const scene::Entity ent =
                    static_cast<scene::Entity>(static_cast<u32>(static_cast<i32>(a.args[0])));
                if (!scene::World::instance().valid(ent)) { w = "no live entity with that id"; return false; }
                const Vec3 p = scene::World::instance().localTransform(ent).position;
                char buf[96];
                std::snprintf(buf, sizeof buf, "(%.3f,%.3f,%.3f)", p.x, p.y, p.z);
                r = buf;
                return true;
            }
#if AVER_MODULE_SCRIPTING
            // load/attach/tick: GraphHost is now hosted from native code via ScriptHost::graphLoad/
            // graphTick (scripts_ member, same seam the drone entity uses in onUpdate -- see
            // setDroneEnabled and the AVER_MODULE_SCRIPTING block right after it). ScriptHost::graphLoad
            // binds a path AND an entity in one call, so "load" just stages the path and "attach" is
            // what actually calls scripts_.graphLoad and reports success/failure honestly.
            if (a.fn == "load") {
                if (a.text.empty()) { w = "load needs a path in \"text\""; return false; }
                if (!scripts_.ready()) {
                    w = "the scripting host is not running: " + scripts_.declineReason();
                    return false;
                }
                if (!scripts_.graphAvailable()) {
                    w = "this build's staged bridge exports no Graph entry points -- rebuild with the "
                        ".NET SDK present so Aver.Scripting.Bridge picks up GraphLoad/GraphTick/GraphUnload";
                    return false;
                }
                mcpGraphPath_ = a.text;
                r = "path staged: " + mcpGraphPath_ + " -- call graph::attach <entityId> to load, "
                    "compile and bind it";
                return true;
            }
            if (a.fn == "attach") {
                if (a.args.empty()) { w = "attach needs an entity id"; return false; }
                if (mcpGraphPath_.empty()) { w = "call graph::load <path> first"; return false; }
                const scene::Entity ent =
                    static_cast<scene::Entity>(static_cast<u32>(static_cast<i32>(a.args[0])));
                if (!scene::World::instance().valid(ent)) { w = "no live entity with that id"; return false; }
                if (!scripts_.graphLoad(static_cast<i32>(ent), mcpGraphPath_)) {
                    w = "graph failed to load/compile from '" + mcpGraphPath_ + "' -- see the [Graph] "
                        "error line just above in the editor log for why";
                    return false;
                }
                mcpGraphEntity_ = ent;
                mcpGraphTimeSeconds_ = 0.0f;
                r = "attached entity #" + std::to_string(static_cast<u32>(ent)) + " to " + mcpGraphPath_;
                return true;
            }
            if (a.fn == "tick") {
                if (a.args.empty()) { w = "tick needs a seconds value (absolute time, not a delta)"; return false; }
                if (mcpGraphEntity_ == scene::kInvalidEntity) {
                    w = "no entity attached -- call graph::attach <entityId> first";
                    return false;
                }
                if (!scene::World::instance().valid(mcpGraphEntity_)) {
                    w = "the attached entity no longer exists";
                    return false;
                }
                mcpGraphTimeSeconds_ = static_cast<f32>(a.args[0]);
                scripts_.graphTick(static_cast<i32>(mcpGraphEntity_), mcpGraphTimeSeconds_);
                const Vec3 p = scene::World::instance().localTransform(mcpGraphEntity_).position;
                char buf[128];
                std::snprintf(buf, sizeof buf, "t=%.3f pos=(%.3f,%.3f,%.3f)",
                              mcpGraphTimeSeconds_, p.x, p.y, p.z);
                r = buf;
                return true;
            }
#else
            if (a.fn == "load" || a.fn == "attach" || a.fn == "tick") {
                w = "this build has no scripting module (AVER_MODULE_SCRIPTING=OFF) -- graph hosting is "
                    "unavailable; entity_pos still works because it reads scene::World directly";
                return false;
            }
#endif
            w = "graph has no entry point '" + a.fn + "'";
            return false;
#else
            (void)a;
            w = "this build has no scene module (AVER_MODULE_SCENE=OFF) -- the graph ABI is unavailable";
            return false;
#endif
        });
    }
#endif // AVER_MODULE_MCP
    bool releasedByUser_ = false;   // Shift+F1 during a session; cleared when the session ends

    // Gives the mouse to the game or hands it back. ShowCursor is a counter, so each call is paired.
    void setMouseCaptured(bool on) {
#if defined(_WIN32)
        if (on == mouseCaptured_) return;
        mouseCaptured_ = on;
        if (on) {
            ShowCursor(FALSE);
            warpToAnchor();
        } else {
            ShowCursor(TRUE);
            ClipCursor(nullptr);
        }
        AVER_INFO("[Sandbox] mouse {} the game{}", on ? "captured by" : "released from",
                  on ? " (Shift+F1 to release)" : "");
#else
        mouseCaptured_ = on;
#endif
    }

#if defined(_WIN32)
    // Parks the cursor at the centre of the window, remembers where that was, and confines it there.
    void warpToAnchor() {
        HWND hwnd = window_ ? static_cast<HWND>(window_->nativeHandle()) : nullptr;
        if (!hwnd) return;
        RECT rc{};
        if (!GetClientRect(hwnd, &rc)) return;
        POINT c{ (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
        ClientToScreen(hwnd, &c);
        captureAnchorX_ = c.x; captureAnchorY_ = c.y;
        SetCursorPos(c.x, c.y);
        RECT screen{};
        POINT tl{ rc.left, rc.top }, br{ rc.right, rc.bottom };
        ClientToScreen(hwnd, &tl); ClientToScreen(hwnd, &br);
        screen.left = tl.x; screen.top = tl.y; screen.right = br.x; screen.bottom = br.y;
        ClipCursor(&screen);
    }

    // Measures one frame of captured mouse movement, then re-centres for the next.
    void pollCapturedMouse() {
        captureDx_ = captureDy_ = 0.0f;
        if (!mouseCaptured_) return;
        POINT p{};
        if (!GetCursorPos(&p)) return;
        captureDx_ = static_cast<f32>(p.x - captureAnchorX_);
        captureDy_ = static_cast<f32>(p.y - captureAnchorY_);
        warpToAnchor();
    }
#else
    void warpToAnchor() {}
    void pollCapturedMouse() { captureDx_ = captureDy_ = 0.0f; }
#endif

    // True while a game is playing, in a build with or without the framework.
    bool playSessionActive() const {
#if AVER_MODULE_FRAMEWORK
        return aver_fw_play_state() != AVER_FW_PLAY_EDITOR;
#else
        return false;
#endif
    }
    // Output Log capture. Written by logSink from any thread under logMutex_, read by the panel.
    static constexpr size_t kMaxLogLines = 4000;
    std::mutex          logMutex_;
    std::deque<LogLine> logLines_;
    bool                logAutoScroll_ = true;
    int                 logLevelFilter_ = 0;      // 0 = all, 1 = Info+, 2 = Warn+
    // Content Browser: the folder whose files are listed, and the Import modal's source-path field.
    std::string         cbSelectedDir_;           // empty -> the content root
    char                importPath_[512] = {};
    bool                cbGallery_ = true;        // tiles vs list
    f32                 cbTileSize_ = 88.0f;      // gallery tile edge, in dp
    std::string         cbSelectedFile_;          // the highlighted entry in the file view
    char                cbFilter_[128] = {};      // the search box: filters the open folder by name
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
    std::string         drawerStartSub_;          // --drawer content:<sub>, applied once at first draw
#if AVER_MODULE_SCENE
    // fnv1a64(asset path) -> mesh handle, for the scene-render pass.
    std::unordered_map<u64, rhi::MeshHandle> sceneMeshes_;
    // Rest bounds per mesh id, in mesh space. Kept beside sceneMeshes_ because CMeshRenderer's own
    // aabb was written as a hardcoded UNIT CUBE at every spawn site and never from the asset -- so
    // picking a 100 cm character meant hitting a 2 cm box at its origin, and culling could not have
    // worked at all. Skinned entities have theirs overwritten per frame by SkinnedScene.
    std::unordered_map<u64, std::pair<Vec3, Vec3>> meshBounds_;
    // Triangle count per mesh id, same key as sceneMeshes_/meshBounds_. Exists so a resident
    // triangle BUDGET can be reported (chunk streaming panel / log) instead of guessed -- a
    // scattered pine forest's cost is invisible without this, and the whole reason a scatter
    // palette needs tuning "by looking" is that triangle count is not visible any other way.
    std::unordered_map<u64, u32> meshTris_;
    int lastSceneDrawn_=-1;           // last scene-entity draw count, so the log line fires only on change
    int lastSceneCulled_=-1;          // and the cull count, so a frustum bug shows as a number rather than a gap
#endif
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    // Per-mesh LOD ladder for virtualized-geometry selection (aver::trifactor::ClusterAdapt/
    // ClusterSelect). Populated in loadProjectMeshes ONLY for a mesh the Cook wrote coarserLods for
    // (lodCount() > 1); a mesh with no entry here has no coarser LOD and always draws its LOD-0
    // handle (already in sceneMeshes_[id]), identically to before this feature existed. Every vector
    // is indexed by LEVEL: [0] mirrors sceneMeshes_[id]/meshTris_[id] exactly (same handle, same
    // count), [i>0] is the Cook's mesh.coarserLods[i-1] -- ITS OWN index buffer, drawn against the
    // SAME vertex buffer level 0 uses (OcMeshData::coarserLods' own comment: every LOD level of a
    // Trifactor DAG shares one vertex stream), so this only ever duplicates INDEX data, never
    // vertices, across the ladder.
    struct MeshLodLadder {
        std::vector<rhi::MeshHandle> handles;   // [level] -> whole-level MeshHandle
        std::vector<u32> triCounts;              // [level] -> that level's own triangle count
        std::vector<f32> errorCm;                // [level] -> aver::trifactor::levelWorldErrorCm(mesh, level)
        // [level] -> that level's meshlets, decoded to ClusterView (bounds + cone only, no vertex
        // data) -- kept resident so the per-frame pass can run the REAL, tested
        // selectVisibleClustersWithStats for informational frustum/cone-cull telemetry on the level
        // actually chosen, without re-parsing the .ocmesh every frame. See onRender's scene-entity
        // pass and ClusterAdapt.hpp's file header for why this culling is counted but NOT (yet)
        // subtracted from what gets drawn in this slice.
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
    // --lod-cluster-stats: OFF by default, on purpose. It gates ONLY the informational per-meshlet
    // frustum/cone-cull telemetry below (a real, extra per-instance CPU cost: transforming a working
    // copy of that level's ClusterViews to world space every frame -- see the comment at its call
    // site). Keeping it separate from lodSelectEnabled_ means the primary --lod-select on/off frame-
    // time comparison measures ONLY the level-selection draw-call change, not this telemetry's own
    // CPU cost on top of it -- the two must not be conflated when reporting numbers.
    bool lodClusterStatsEnabled_ = false;
    // This frame's selection counters, logged under the "[LOD-SELECT]" tag (greppable) whenever any
    // of them changes -- see the brief's "report the counters" requirement. trianglesBeforeLod0 vs
    // trianglesAfterLevel is the number that actually predicts frame time (every triangle counted in
    // "after" really reaches a drawMesh call this frame); the cluster-cull counters are real,
    // measured telemetry from ClusterSelect's own tested functions but are NOT yet subtracted from
    // "after" -- see the file header on why (per-level, not per-cluster, selection this slice).
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

    // ============================================================================================
    // PER-CLUSTER selection -- what actually makes this virtualized geometry instead of discrete LOD
    // with generated levels. See aver::trifactor::ClusterAdapt.hpp's "PER-CLUSTER, at last" section.
    // Switchable against the per-level path above (--lod-per-cluster) so before/after is one flag on
    // top of the SAME build/binary, not a separate compile -- both paths ship in every binary and the
    // flag decides which one an instance uses at draw time.
    // ============================================================================================

    // Every meshlet of a mesh, across EVERY LOD level at once, mesh-local space, plus its expanded
    // GLOBAL triangle-index list -- built once at load time (loadProjectMeshes, same gate as
    // MeshLodLadder: only for a mesh the Cook wrote coarserLods for). `verts` is a COPY of the same
    // vertex array createMesh was first called with; every cluster from every level indexes it
    // (OcMeshData::coarserLods' shared-vertex-buffer contract), so ONE copy serves the whole DAG and
    // every cut this mesh's instances can ever select.
    struct MeshClusterData {
        std::vector<trifactor::MeshClusterView> clusters;   // mesh-local; all levels
        std::vector<std::vector<u32>> clusterIndices;        // [clusterId] -> expanded global indices
        std::vector<rhi::MeshVertex> verts;

        // THE INSTANCE-LEVEL SHORTCUT's own precomputed inputs -- see
        // aver::trifactor::ClusterAdapt.hpp's "THE INSTANCE-LEVEL SHORTCUT" section for the full
        // derivation. Both built ONCE here, from `clusters` right above, immediately after
        // buildMeshClusterViews -- never touched again per frame; the runtime cost of computing them
        // is O(clusters.size()), paid once at load, the same place `clusters` itself gets built.
        std::vector<trifactor::MeshClusterLevelBounds> levelBounds;
        f32 maxSphereRadius = 0.0f;   // mesh-local; largest cluster sphere radius across the whole DAG
    };
    std::unordered_map<u64, MeshClusterData> meshClusterData_;

    // ONE cut-assembled MeshHandle PER INSTANCE (not per mesh -- two instances of the same mesh at
    // different distances select different clusters), rebuilt only when the selected cluster-id set
    // actually changes -- "cache the cut and rebuild only when it changes" per the task brief. Keyed
    // by scene::Entity because that is what is stable across frames for a CMeshRenderer instance;
    // `lastUsedFrame` is how staleEntityCacheSweep below reclaims a handle whose entity stopped
    // appearing (destroyed, or its component removed) without needing every entity's destruction
    // hooked explicitly.
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
    // watch -- see modules/trifactor/include/aver/trifactor/ClusterAdapt.hpp and the task's own
    // "MEASURE the upload" instruction. Real std::chrono timing around every createMesh call this
    // path makes, not a guess.
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

    // ============================================================================================
    // GPU per-cluster LOD -- --lod-mesh-shader. An amplification shader (ASMain) runs the SAME local
    // cut test as selectClusterLocal/inLocalCut above, one thread per cluster, and DispatchMesh's the
    // survivors; a mesh shader (MSClusterMain) expands each surviving cluster's own vertex/triangle
    // block. See modules/rhi/src/RHIShaders.cpp's AVER_MS_CLUSTER block for the shaders themselves and
    // modules/trifactor/include/aver/trifactor/ClusterAdapt.hpp's buildMeshClusterGpuData for the
    // upload shape. Exists ALONGSIDE the CPU per-cluster path above (never replaces it): the CPU path
    // stays the correctness reference the two are checked to agree with, per the task's own
    // instruction, and is what a mesh-shader-tier-0 device still runs when this flag is on.
    // ============================================================================================

    // Per-mesh GPU cluster buffers, built once at load time (loadProjectMeshes) alongside
    // MeshClusterData -- ONLY when --lod-mesh-shader is on, so a run that never asks for this feature
    // never pays for the extra upload. `clusterCount` is outBounds.size() from
    // buildMeshClusterGpuData -- every level's clusters, concatenated, exactly like the CPU
    // MeshClusterData's own `clusters` array covers.
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
    rhi::ShaderHandle lodMeshAsShader_ = 0, lodMeshMsShader_ = 0, lodMeshPsShader_ = 0;
    rhi::PipelineHandle lodMeshPipeline_ = 0;
    rhi::PipelineLayout lodMeshLayout_{};   // srvCount=4 (the four cluster buffers); kept so
                                             // meshGeometryDefines(layout) at draw time (were it ever
                                             // needed again) agrees with what compiled the shaders.

    // Per-instance constant block for ASMain/MSClusterMain beyond gWorld/gViewProj/gCamPos --
    // byte-for-byte the HLSL ClusterFrameCB (RHIShaders.cpp), bound as a root CBV at b4
    // (kFeatureFrameConstantRegister). `planes` is aver::trifactor::Frustum::fromViewProj's own
    // output, copied verbatim -- see that struct's `plane[6][4]` -- so the GPU test runs against the
    // IDENTICAL six numbers the CPU reference tests against, not a second derivation of them.
    struct ClusterFrameCB {
        f32 budgetPx = 0.0f;
        f32 projScale = 0.0f;
        f32 worldScale = 1.0f;
        u32 _pad = 0;
        f32 planes[6][4] = {};
    };

    // Informational counters for --lod-mesh-shader, logged under "[LOD-MESH-SHADER]" (greppable, same
    // log-on-change discipline as the other two paths). NOT a GPU readback this slice: computed by
    // running the SAME, already-tested CPU reference (trifactor::selectClusterCut) over the SAME
    // world-space clusters and the SAME budget the GPU dispatch just used, purely for telemetry --
    // never reaching drawMesh, exactly the "informational, real, tested" discipline the per-level
    // path's own cluster telemetry above already uses. Because ASMain/MSClusterMain are byte-for-byte
    // ports of inLocalCut/coneCull/Frustum::intersectsSphere (RHIShaders.cpp's AVER_MS_CLUSTER block
    // comments say so at each function), what this counts is what the GPU dispatch actually drew, NOT
    // a separate estimate that merely correlates with it -- but it is still the CPU arithmetic that
    // produced the number, not bytes read back from the GPU that ran it. Said plainly in the report,
    // not just here.
    struct LodMeshShaderStats {
        u32 instancesTested = 0;
        u32 clustersDispatched = 0;      // sum of MeshClusterGpu::clusterCount over drawn instances
        u32 survivors = 0;               // sum of survivor counts (CPU-mirrored, see struct comment)
        u64 trianglesDrawn = 0;
        u32 instancesMixedLevels = 0;
        u32 maxDistinctLevelsSeen = 0;
        u32 instancesShortcut = 0;       // of the sampled instances, how many used
                                          // provablySingleLevelCut instead of the real CPU-mirror scan
                                          // -- see the sampling block's own comment for why this matters
                                          // far more here than in the CPU per-cluster path's own count:
                                          // without it, this "sampled every 64 frames" telemetry was
                                          // itself a multi-SECOND stall once every 64 frames.
    };
    LodMeshShaderStats lodMeshShaderStats_{};
    LodMeshShaderStats lastLoggedLodMeshShaderStats_{};
#endif

    // OUTSIDE EVERY MODULE GUARD, and it has to be. The map is POPULATED beside the Trifactor LOD
    // ladder, but it is READ from the unguarded call that hands the resolver to Voxi -- so declaring
    // it next to what fills it put the member behind AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR and
    // broke both scene-off and trifactor-off. Same mistake, same session, second time: the guard
    // belongs where a thing is BUILT, never where it is declared, whenever something unguarded can
    // still ask for it.
    //
    // Degrading is the point. With no Trifactor the map is simply empty, every lookup answers 0, and
    // every pass draws the mesh it was given -- which is exactly what those builds did before.
    std::unordered_map<rhi::MeshHandle, rhi::MeshHandle> depthProxy_;

    static rhi::MeshHandle depthProxyLookup(rhi::MeshHandle mesh, void* user) {
        const auto& m = static_cast<const SandboxApp*>(user)->depthProxy_;
        const auto it = m.find(mesh);
        return it == m.end() ? 0 : it->second;
    }

    u32 sceneWalkReports_ = 0;     // scene walks so far; the cost split reports at 2^n of them
    u32 chunkStreamReports_ = 0;   // ditto, for the streamer's main-thread cost

    Mat4 invVP_, viewProj_; Vec3 eye_{0,0,0};
};

// True for a path ending in ".ocproject", case-insensitively.
static bool isOcproject(const char* p) {
    const usize n = std::strlen(p);
    if (n < 11) return false;
    const char* ext = p + n - 10;
    static const char* kExt = ".ocproject";
    for (int i = 0; i < 10; ++i) {
        char a = ext[i], b = kExt[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

// Parses the command line and builds the editor application. Some flags do their work and exit.
Application* createApplication(int argc, char** argv) {
    u16 mcpPort=0;
    // --mcp was given at all, vs. given WITH an explicit port. Precedence (explicit CLI argument >
    // mcp.conf > built-in default) needs to tell those apart: an explicit numeric port is resolved
    // right here and nothing else may override it; --mcp with no number defers to mcp.conf, resolved
    // once argument parsing is done and engineRoot() can be asked (see mcpRequested below).
    bool mcpRequested = false, mcpPortExplicit = false;
    u64 frames=0; bool headless=false, focusVoxi=false, focusScript=false, focusTools=false, focusCompileMenu=false, focusCompile=false, startScreen=false; int drawerOpen=0; std::string drawerSub; std::string beam, shot, project, scriptsDir, spawnTest; bool playTest=false; bool skinTest=false; bool skinDrawTest=false; bool particleTest=false; bool noParticleGi=false; int particleStressEmitters=0; int particleStressMaxParticles=0; bool particleStressSecondEmitter=false; bool reflTest=false; bool furnaceTest=false; bool furnaceSun=false; bool ptFurnace=false; bool ptScene=false; int ptSceneToggleOn=0; int ptSceneToggleOff=0; int projectSettingsPage=-1; f32 sunAngle=-1.0f; std::string skinSceneDir; Tool tool=Tool::Select; int msaa=0; int gi=0; int rt=0; int rtRays=0; int rtPixelsPerRay=0; int rtShadowDenoise=-1; int giUpdateInterval=0; f32 renderScale=1.0f; std::string aversrArg; bool frameTime=false; bool noGi=false; bool noRt=false; bool giConeOff=false; f32 camWobbleDeg=0.0f; int camWobblePeriod=0; bool giDbg=false, ms=false; u32 probeX=0, probeY=0; f32 probeU=-1.0f, probeV=-1.0f; bool camSet=false; f32 camX=0, camY=0, camZ=0, camPitch=0, camYaw=0; int reloadAt=0; bool warp=false, debugLayer=false; std::string backendName; const char* forceCaps=nullptr; f32 bloom=0.0f, exposure=1.0f; bool autoExposure=false; int clouds=0; f32 cloudCover=-1.0f; bool skyPhysical=false, skyAuthored=false; f32 skyElevation=-999.0f; bool vsyncOff=false; bool uiDemo=false; bool inputProbe=false; bool autoCompile=false; bool showPrefs=false; bool scrollPrefsToKeybinds=false; bool saveProject=false; std::string importSrc, importDst; int focusLevelAt=0; int hudTest=-1; std::string openAsset; std::string graphSelectNode; std::string graphTab; int chunkStream=0; int droneAuto=0; int undoTestAuto=0; int keybindTestAuto=0; std::string keybindTestMode; std::string droneGraph; std::string landscapePath; bool fogMatch=false; f32 fogMatchOpacity=-1.0f; bool lodSelect=true; f32 lodErrorPx=1.0f; bool lodClusterStats=false; bool lodPerCluster=false; int lodMeshShader=-1; bool depthPrepass=false; bool edgeAa=false; bool occlusionCull=false;
    for (int i=1;i<argc;++i){
        // --new-project <location> <name> scaffolds a project and exits, touching no device.
        if (!std::strcmp(argv[i],"--new-project") && i+2<argc) {
            const std::string loc = argv[++i], nm = argv[++i];
            fmt::ProjectDesc made;
            std::string why;
            if (editor::scaffoldProject(loc, nm, made, &why)) {
                AVER_INFO("[Sandbox] scaffolded '{}' at {}", nm, made.dir);
                std::exit(0);
            }
            AVER_ERROR("[Sandbox] could not scaffold '{}': {}", nm, why);
            std::exit(1);
        }
        // --new-project-template <location> <name> <templateId> scaffolds a project from a shipped
        // template and exits, touching no device -- the exact --new-project precedent just above,
        // for the branch the New Project modal's template picker calls. Needs no window or device
        // because listTemplates()/scaffoldProjectFromTemplate() are pure filesystem, same as
        // scaffoldProject() itself.
        else if (!std::strcmp(argv[i],"--new-project-template") && i+3<argc) {
            const std::string loc = argv[++i], nm = argv[++i], tmplId = argv[++i];
            const std::vector<editor::TemplateInfo> tmpls = editor::listTemplates();
            const editor::TemplateInfo* found = nullptr;
            for (const editor::TemplateInfo& t : tmpls) if (t.id == tmplId) { found = &t; break; }
            if (!found) {
                AVER_ERROR("[Sandbox] no template named '{}' ({} found)", tmplId, tmpls.size());
                std::exit(1);
            }
            fmt::ProjectDesc made;
            std::string why;
            if (editor::scaffoldProjectFromTemplate(loc, nm, *found, made, &why)) {
                AVER_INFO("[Sandbox] scaffolded '{}' from template '{}' at {}", nm, tmplId, made.dir);
                std::exit(0);
            }
            AVER_ERROR("[Sandbox] could not scaffold '{}' from template '{}': {}", nm, tmplId, why);
            std::exit(1);
        }
        // --upgrade-project <path.ocproject> applies what the prompt would apply, and exits.
        else if (!std::strcmp(argv[i],"--upgrade-project") && i+1<argc) {
            const std::string manifest = argv[++i];
            fmt::ProjectDesc p;
            std::string why;
            if (!fmt::loadOcproject(manifest, p, &why)) {
                AVER_ERROR("[Sandbox] could not load '{}': {}", manifest, why);
                std::exit(1);
            }
            const editor::ProjectUpgrade up = editor::inspectProject(p);
            if (up.empty()) { AVER_INFO("[Sandbox] '{}' is already current", p.name); std::exit(0); }
            for (const editor::ProjectFix& f : up.fixes)
                AVER_INFO("  {} : {}", f.summary, f.detail);
            if (!editor::applyProjectUpgrade(p, up, &why)) {
                AVER_ERROR("[Sandbox] upgrade failed: {}", why);
                std::exit(1);
            }
            std::exit(0);
        }
        // --landscape-gen <path> [sampleCount] [spacingCm] writes a synthetic rolling-hill .ocland
        // through the real writeOcLand/loadOcLand round trip and exits, touching no device. Exists
        // because no .ocland fixture exists anywhere -- the format's own reader/writer is the only
        // thing this repo has ever produced one with (tests/landscape builds one in memory and never
        // saves it) -- and a hand-rolled binary fixture would prove nothing about the real format.
        // sampleCount must be (k * 64) + 1 (LandscapeTree::build's own acceptance rule); the default,
        // 257, is 4*64+1 -- three LOD levels, small enough to build and upload in a --frames run.
        else if (!std::strcmp(argv[i],"--landscape-gen") && i+1<argc) {
            const std::string outPath = argv[++i];
            u32 samples = 257; f32 spacingCm = 400.0f;
            if (i+1 < argc && argv[i+1][0] != '-') samples = static_cast<u32>(std::atoi(argv[++i]));
            if (i+1 < argc && argv[i+1][0] != '-') spacingCm = static_cast<f32>(std::atof(argv[++i]));
#if AVER_MODULE_LANDSCAPE
            fmt::OcLandData d;
            d.sampleCount = samples;
            d.spacingCm = spacingCm;
            const f32 extentCm = static_cast<f32>(samples - 1) * spacingCm;
            // CENTRED ON THE ORIGIN, so the section sits under wherever a level's own placements
            // already are rather than requiring the level to be authored around the terrain instead.
            d.originCm[0] = -extentCm * 0.5f; d.originCm[1] = -extentCm * 0.5f; d.originCm[2] = 0.0f;
            d.heights.resize(static_cast<usize>(samples) * samples);
            for (u32 iy = 0; iy < samples; ++iy) {
                for (u32 ix = 0; ix < samples; ++ix) {
                    const f32 fx = static_cast<f32>(ix), fy = static_cast<f32>(iy);
                    // Two overlapping sine fields, index-frequency (not world-frequency): rolling
                    // hills with real relief at any spacing, so --frames LOD selection has something
                    // to refine and coarsen against, not a plane a first-time reader could mistake for
                    // a bug.
                    const f32 h = 1400.0f * std::sin(fx * 0.045f) * std::cos(fy * 0.037f)
                                + 700.0f  * std::sin((fx + fy) * 0.021f);
                    d.heights[static_cast<usize>(iy) * samples + ix] = h;
                }
            }
            std::string why;
            if (fmt::saveOcLand(outPath, d, &why)) {
                AVER_INFO("[Sandbox] wrote {} ({}x{} samples, {:.0f} m across)",
                          outPath, samples, samples, extentCm / 100.0f);
                std::exit(0);
            }
            AVER_ERROR("[Sandbox] could not write '{}': {}", outPath, why);
            std::exit(1);
#else
            (void)samples; (void)spacingCm;
            AVER_ERROR("[Sandbox] --landscape-gen needs AVER_MODULE_LANDSCAPE (this build has it OFF): '{}' not written", outPath);
            std::exit(1);
#endif
        }
        // --sculpt-test <in.ocland> <out.ocland> [mode] [radiusCm] [strengthCm] applies one brush
        // stroke (several ticks, the same shape a held mouse button produces -- see handleSculpt's
        // own amount-per-tick, here fixed rather than dt-derived so the result is reproducible)
        // through the EXACT functions the editor's own sculpt tools call -- landscape::applyBrush
        // and fmt::saveOcLand -- centred on the section's own middle, then exits touching no device.
        // Exists for the same reason --landscape-gen does: "a sculpt changes the stored heights"
        // needs a real .ocland round trip, not a claim about code nobody ran.
        else if (!std::strcmp(argv[i],"--sculpt-test") && i+2<argc) {
            const std::string sculptIn = argv[++i], sculptOut = argv[++i];
            std::string sculptMode = "raise"; f32 sculptRadiusArg = 0.0f, sculptStrengthArg = 0.0f;
            if (i+1 < argc && argv[i+1][0] != '-') sculptMode = argv[++i];
            if (i+1 < argc && argv[i+1][0] != '-') sculptRadiusArg = static_cast<f32>(std::atof(argv[++i]));
            if (i+1 < argc && argv[i+1][0] != '-') sculptStrengthArg = static_cast<f32>(std::atof(argv[++i]));
#if AVER_MODULE_LANDSCAPE
            fmt::OcLandData sd;
            std::string sculptWhy;
            if (!fmt::loadOcLand(sculptIn, sd, &sculptWhy)) {
                AVER_ERROR("[Sandbox] --sculpt-test could not load '{}': {}", sculptIn, sculptWhy);
                std::exit(1);
            }
            const landscape::BrushMode sculptModeVal =
                sculptMode == "lower"   ? landscape::BrushMode::Lower
              : sculptMode == "smooth"  ? landscape::BrushMode::Smooth
              : sculptMode == "flatten" ? landscape::BrushMode::Flatten
                                        : landscape::BrushMode::Raise;
            landscape::BrushParams sp;
            sp.centerCm[0] = sd.originCm[0] + sd.extentCm() * 0.5f;
            sp.centerCm[1] = sd.originCm[1] + sd.extentCm() * 0.5f;
            sp.radiusCm = sculptRadiusArg > 0.0f ? sculptRadiusArg : sd.extentCm() * 0.2f;
            sp.strength = sculptStrengthArg > 0.0f ? sculptStrengthArg : 300.0f;
            sp.mode = sculptModeVal;
            const u32 cix = (sd.sampleCount - 1) / 2, ciy = cix;
            sp.flattenTargetCm = sd.heightAt(cix, ciy) + 500.0f;   // only read by Flatten
            const f32 sculptBefore = sd.heightAt(cix, ciy);
            // Eight ticks at amount 0.25, like ~130ms of a held mouse button at the dt*6 rate
            // handleSculpt uses -- not one amount=1 jump, so the result actually depends on
            // strength/radius rather than degenerating to "did anything change at all".
            landscape::BrushRect sculptRect{};
            for (int tick = 0; tick < 8; ++tick) sculptRect = landscape::applyBrush(sd, sp, 0.25f);
            if (sculptRect.empty) {
                AVER_ERROR("[Sandbox] --sculpt-test: the brush touched no sample -- radius/centre "
                           "landed entirely off the section");
                std::exit(1);
            }
            const f32 sculptAfter = sd.heightAt(cix, ciy);
            if (!fmt::saveOcLand(sculptOut, sd, &sculptWhy)) {
                AVER_ERROR("[Sandbox] --sculpt-test could not write '{}': {}", sculptOut, sculptWhy);
                std::exit(1);
            }
            fmt::OcLandData sculptBack;
            if (!fmt::loadOcLand(sculptOut, sculptBack, &sculptWhy)) {
                AVER_ERROR("[Sandbox] --sculpt-test could not read back '{}': {}", sculptOut, sculptWhy);
                std::exit(1);
            }
            const f32 sculptSaved = sculptBack.heightAt(cix, ciy);
            AVER_INFO("[Sandbox] --sculpt-test {} on '{}': centre sample {:.2f} -> {:.2f} cm in "
                      "memory, {:.2f} cm after save+reload, touched rect [{},{}]-[{},{}]",
                      sculptMode, sculptIn, sculptBefore, sculptAfter, sculptSaved,
                      sculptRect.x0, sculptRect.y0, sculptRect.x1, sculptRect.y1);
            std::exit(0);
#else
            (void)sculptIn; (void)sculptMode; (void)sculptRadiusArg; (void)sculptStrengthArg;
            AVER_ERROR("[Sandbox] --sculpt-test needs AVER_MODULE_LANDSCAPE (this build has it OFF): "
                       "'{}' not written", sculptOut);
            std::exit(1);
#endif
        }
        // --open-asset <path> opens a file through the same host a double-click goes through.
        else if (!std::strcmp(argv[i],"--open-asset") && i+1<argc) openAsset=argv[++i];
        // --graph-tab viewport: front the graph editor's inner Viewport tab (component tree +
        // preview) so a capture run can prove it draws.
        else if (!std::strcmp(argv[i],"--graph-tab") && i+1<argc) graphTab=argv[++i];
        // --graph-select <nodeId>: select a node in the just-opened .ocgraph, one frame later -- see
        // SandboxApp::setGraphSelectNode's own comment for why.
        else if (!std::strcmp(argv[i],"--graph-select") && i+1<argc) graphSelectNode=argv[++i];
        // --actor-live brings an actor tab up with LIVE already on.
        else if (!std::strcmp(argv[i],"--actor-live")) editor::setActorEditorLiveByDefault(true);
        else if (!std::strcmp(argv[i],"--headless")) headless=true;
        else if (!std::strcmp(argv[i],"--input-probe")) inputProbe=true;
        else if (!std::strcmp(argv[i],"--auto-compile")) autoCompile=true;
        else if (!std::strcmp(argv[i],"--focus-level-at") && i+1<argc) focusLevelAt=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--project-settings")) focusVoxi=true;
        // --project-settings-page N: verification-only, see setProjectSettingsPage's own comment.
        else if (!std::strcmp(argv[i],"--project-settings-page") && i+1<argc) projectSettingsPage=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--editor-prefs")) showPrefs=true;
        // --scroll-prefs-to-keybinds: see buildEditorPrefs()'s own comment on the flag it sets.
        else if (!std::strcmp(argv[i],"--scroll-prefs-to-keybinds")) scrollPrefsToKeybinds=true;
        else if (!std::strcmp(argv[i],"--hud-preview") && i+1<argc) hudTest=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--save-project")) saveProject=true;
        else if (!std::strcmp(argv[i],"--import") && i+2<argc) { importSrc=argv[++i]; importDst=argv[++i]; }
        else if (!std::strcmp(argv[i],"--new-script")) focusScript=true;
        else if (!std::strcmp(argv[i],"--tools-menu")) focusTools=true;
        else if (!std::strcmp(argv[i],"--compile-menu")) focusCompileMenu=true;
        // --mcp [port] opens the editor control channel. Opt-in: it is a listening socket. With no
        // number, the port comes from mcp.conf's editor_bridge.port, or 45123 if that is absent too
        // -- resolved after this loop, once mcpRequested/mcpPortExplicit are both known for good.
        else if (!std::strcmp(argv[i],"--mcp")) {
            mcpRequested = true;
            if (i+1 < argc && argv[i+1][0] != '-') {
                // RANGE-CHECKED, unlike the bare `(u16)std::atoi(...)` this replaces. That cast
                // silently truncated: `--mcp 99999` bound port 34463 (99999 mod 65536) and logged
                // 34463 as though the operator had typed it, and `--mcp 0` counted as an explicit
                // port that then never opened a channel at all, because 0 is also this parser's
                // "never asked for" value. Both are the same failure the mcp.conf reader already
                // refuses to commit on its own side -- a port that is not a port should be said
                // out loud, not quietly turned into a different one.
                const char* raw = argv[++i];
                const long  p   = std::strtol(raw, nullptr, 10);
                if (p >= 1 && p <= 65535) {
                    mcpPort = (u16)p;
                    mcpPortExplicit = true;
                } else {
                    // Not fatal, and deliberately not: the channel is a debugging aid, and refusing
                    // to start the whole editor over a mistyped port would be worse than falling
                    // back the way every other unset value here does.
                    AVER_WARN("[Mcp] --mcp {} is not a port (1-65535); falling back to mcp.conf or "
                              "the built-in default", raw);
                }
            }
        }
        else if (!std::strcmp(argv[i],"--compile-scripts")) focusCompile=true;
        // --reload-scripts [N] fires Tools > Reload Scripts once, N frames in (default 20).
        else if (!std::strcmp(argv[i],"--reload-scripts")) {
            reloadAt = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 20;
        }
        else if (!std::strcmp(argv[i],"--start-screen")) startScreen=true;
        // --chunk-stream [N] switches chunk streaming on N frames in (default 5), the same "wait a
        // few frames for the project/scene to settle" pattern --reload-scripts uses. Exists so a
        // --frames capture run can prove streaming happened without a human clicking the menu item.
        // --drone-graph <path> names the .ocgraph the drone runs, relative to Content. Without it
        // the drone spawns and sits still, which is the honest behaviour for an engine that does
        // not know what any project's scripts are called.
        else if (!std::strcmp(argv[i],"--drone-graph") && i+1<argc) droneGraph = argv[++i];
        // --landscape <path.ocland> overrides the levelname.ocland convention loadLandscapeForLevel
        // otherwise derives from whatever level loads next. Exists so a --frames capture run can
        // prove the render and per-frame LOD-selection path draws real terrain without a level file
        // that names one and without a human clicking anything -- see loadLandscapeForLevel.
        else if (!std::strcmp(argv[i],"--landscape") && i+1<argc) landscapePath = argv[++i];
        else if (!std::strcmp(argv[i],"--chunk-stream")) {
            chunkStream = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 5;
        }
        // --no-chunk-stream: the counterpart to streaming now being ON by default. A negative value
        // is the "explicitly off" signal, distinct from the 0 that means "the flag was not given" --
        // without that distinction the app cannot tell a user who wants a static scene from one who
        // said nothing, and the default would be unturnoffable. Wanted by anything measuring a fixed
        // scene: a frame-time comparison whose triangle count is still climbing is not a comparison.
        else if (!std::strcmp(argv[i],"--no-chunk-stream")) chunkStream = -1;
        // --fog-match [opacity] ties fog density to the streaming radius, the same thing the Height
        // Fog panel's checkbox does. A flag as well as a checkbox because the feature is invisible
        // without one: it is opt-in by design (it makes the world markedly foggier), so a headless
        // run could never exercise it, and a verifier reasonably reported it as inert.
        else if (!std::strcmp(argv[i],"--fog-match")) {
            fogMatch = true;
            if (i+1 < argc && argv[i+1][0] != '-') fogMatchOpacity = static_cast<f32>(std::atof(argv[++i]));
        }
        // --drone [N] switches the graph-driven drone on N frames in (default 5), same shape and
        // reason as --chunk-stream just above: proves it headlessly without a human clicking the menu.
        else if (!std::strcmp(argv[i],"--drone")) {
            droneAuto = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 5;
        }
        // --undo-test [N]: fires runUndoTest() N frames in (default 10), then EXITS THE PROCESS
        // with 0/1 -- see runUndoTest()'s own comment. Same "wait a few frames to settle" shape as
        // --chunk-stream/--drone, just with a longer default: it needs a live scene::World, and
        // giving the rest of startup a few extra frames costs nothing in a one-shot test run.
        else if (!std::strcmp(argv[i],"--undo-test")) {
            undoTestAuto = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 10;
        }
        // --keybind-test write|read [N]: fires runKeybindPersistTest(mode) N frames in (default 10),
        // then EXITS THE PROCESS with 0/1 -- see that function's own comment.
        else if (!std::strcmp(argv[i],"--keybind-test") && i+1<argc) {
            keybindTestMode = argv[++i];
            keybindTestAuto = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 10;
        }
        // --drawer log|content[:<sub>] opens a bottom drawer, optionally in a Content subfolder.
        else if (!std::strcmp(argv[i],"--drawer") && i+1<argc) {
            const char* v = argv[++i];
            drawerOpen = !std::strcmp(v,"log") ? 2 : 1;
            if (const char* colon = std::strchr(v, ':')) drawerSub = colon + 1;
        }
        else if (!std::strcmp(argv[i],"--msaa") && i+1<argc) msaa=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--gi")) gi=3;
        else if (!std::strcmp(argv[i],"--no-gi")) noGi=true;
        else if (!std::strcmp(argv[i],"--gi-debug")) { gi=3; giDbg=true; }
        // --no-gi-cone: the A/B measurement toggle -- see VoxiRenderer::setConeTraceEnabled and
        // SandboxApp::setGiConeTraceOff's own comments. Distinct from --no-gi, which also stops the
        // volume from being built; this only stops PSMainVoxi/PSClusterMain from READING it.
        else if (!std::strcmp(argv[i],"--no-gi-cone")) giConeOff=true;
        else if (!std::strcmp(argv[i],"--rt")) rt=3;
        // The Off rung of the ray-tracing cost ladder. It needs its own flag because RT is on by
        // default now and --rt cannot express Off: 0 is rtOverride_'s "not given".
        else if (!std::strcmp(argv[i],"--no-rt")) noRt=true;
        // --cam-wobble DEG PERIOD: see setCamWobble. Measurement-only; 0 degrees is no motion,
        // so every existing capture is bit-identical without it.
        else if (!std::strcmp(argv[i],"--cam-wobble") && i+2<argc) {
            camWobbleDeg=(f32)std::atof(argv[++i]); camWobblePeriod=std::atoi(argv[++i]);
        }
        // The sun occlusion rays per pixel, so the cost of ray-traced shadows can be MEASURED
        // instead of asserted: the sequence is nested, so 1, 2, 4, 8 is one converging series.
        else if (!std::strcmp(argv[i],"--rt-rays") && i+1<argc) rtRays=std::atoi(argv[++i]);
        // The ray-traced shadow's temporal amortisation tile edge -- how many pixels share one
        // traced ray, rounded to the nearest power of two. 1 (unset) traces every pixel every frame.
        else if (!std::strcmp(argv[i],"--rt-pixels-per-ray") && i+1<argc) rtPixelsPerRay=std::atoi(argv[++i]);
        // The SPATIAL filter radius in pixels, 0 = off. Distinct from the tile edge above in every
        // way that matters: that one amortises over TIME and lags the camera, this one averages
        // over SPACE and keeps no history at all.
        else if (!std::strcmp(argv[i],"--rt-shadow-denoise") && i+1<argc) rtShadowDenoise=std::atoi(argv[++i]);
        // How many frames apart the GI volume is revoxelised -- 1 (unset) rebuilds every frame, the
        // original always-fresh behaviour. Measures the voxelise+filter amortisation independently of
        // everything else, per the "measure each change, do not stack guesses" rule.
        else if (!std::strcmp(argv[i],"--gi-update-interval") && i+1<argc) giUpdateInterval=std::atoi(argv[++i]);
        // The scene's own render resolution as a fraction of the present/swapchain size -- 1.0
        // (unset) reproduces the pre-existing 1:1 behaviour exactly. Clamped to [0.25,1] by the
        // device; measures the render-scale/GI-cost tradeoff independently of everything else.
        else if (!std::strcmp(argv[i],"--render-scale") && i+1<argc) renderScale=static_cast<f32>(std::atof(argv[++i]));
        // --aversr LEVEL: off|quality|balanced|performance (case-insensitive), the docs/AVERSR.md
        // "Quality levels" table. Stored as a string here and parsed/applied below (after the loop,
        // alongside every other app->setXxx call) rather than inline, so a build with the module
        // compiled out can still recognise the flag and explain why it did nothing rather than
        // erroring as unknown -- see the AVER_MODULE_SR branch after the parse loop.
        else if (!std::strcmp(argv[i],"--aversr") && i+1<argc) aversrArg=argv[++i];
        // --depth-prepass: same-frame depth-only pass ahead of the opaque colour walk, so an
        // occluded fragment skips PSMainVoxi's shadow lookup/cone trace/fog entirely. Unset (the
        // default) reproduces pre-existing behaviour exactly -- see setDepthPrepassOverride's comment.
        else if (!std::strcmp(argv[i],"--depth-prepass")) depthPrepass=true;
        // --occlusion-cull: hierarchical-Z two-pass box culling (modules/occlusion). Unset (the
        // default) reproduces pre-existing behaviour exactly, bit for bit -- see
        // setOcclusionCullOverride's own comment, same "off is today's frame back, byte for byte"
        // contract --depth-prepass and --edge-aa both already carry.
        else if (!std::strcmp(argv[i],"--occlusion-cull")) occlusionCull=true;
        // --edge-aa: FxaaResolve through the SAME rhi::IUpscaler seam --aversr uses -- see
        // edgeAaEnabled_'s own comment for how the two share one slot. A SETTING, not a hard
        // replacement for MSAA: it runs whatever sample count --msaa already asked for.
        else if (!std::strcmp(argv[i],"--edge-aa")) edgeAa=true;
        else if (!std::strcmp(argv[i],"--frame-time")) frameTime=true;
        else if (!std::strcmp(argv[i],"--ms")) ms=true;
        else if (!std::strcmp(argv[i],"--probe") && i+2<argc) { probeX=(u32)std::atoi(argv[++i]); probeY=(u32)std::atoi(argv[++i]); }
        // --force-caps clamps what the device reports; it can never raise a capability.
        else if (!std::strcmp(argv[i],"--force-caps") && i+1<argc) forceCaps=argv[++i];
        else if (!std::strcmp(argv[i],"--warp")) warp=true;
        else if (!std::strcmp(argv[i],"--backend") && i+1<argc) backendName=argv[++i];
        else if (!std::strcmp(argv[i],"--debug-layer")) debugLayer=true;
        else if (!std::strcmp(argv[i],"--scripts") && i+1<argc) scriptsDir=argv[++i];
        else if (!std::strcmp(argv[i],"--spawn-test") && i+1<argc) spawnTest=argv[++i];
        else if (!std::strcmp(argv[i],"--play-test")) playTest=true;
        else if (!std::strcmp(argv[i],"--skin-test")) skinTest=true;
        else if (!std::strcmp(argv[i],"--skin-draw-test")) skinDrawTest=true;
        else if (!std::strcmp(argv[i],"--particle-test")) particleTest=true;
        // --no-particle-gi: see SandboxApp::setNoParticleGi's own comment.
        else if (!std::strcmp(argv[i],"--no-particle-gi")) noParticleGi=true;
        // --particle-stress <N> <M>: VERIFICATION-ONLY, see setParticleStress's own comment.
        else if (!std::strcmp(argv[i],"--particle-stress") && i+2<argc) {
            particleStressEmitters=std::atoi(argv[++i]);
            particleStressMaxParticles=std::atoi(argv[++i]);
        }
        else if (!std::strcmp(argv[i],"--particle-stress2")) particleStressSecondEmitter=true;
        else if (!std::strcmp(argv[i],"--refl-test")) reflTest=true;
        else if (!std::strcmp(argv[i],"--furnace-test")) furnaceTest=true;
        else if (!std::strcmp(argv[i],"--furnace-sun")) furnaceSun=true;
        else if (!std::strcmp(argv[i],"--sun-angle") && i+1<argc) sunAngle=(f32)std::atof(argv[++i]);
        else if (!std::strcmp(argv[i],"--pt-furnace")) ptFurnace=true;
        else if (!std::strcmp(argv[i],"--pt-scene")) ptScene=true;
        // --pt-scene-toggle-on/-off [N]: verification-only, see SandboxApp::ptSceneToggleOnAutoFrames_
        // for what this proves and why. Same "[N] optional, default given" shape as --chunk-stream.
        else if (!std::strcmp(argv[i],"--pt-scene-toggle-on")) {
            ptSceneToggleOn = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 5;
        }
        else if (!std::strcmp(argv[i],"--pt-scene-toggle-off")) {
            ptSceneToggleOff = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 15;
        }
        else if (!std::strcmp(argv[i],"--skin-scene-test") && i+1<argc) skinSceneDir=argv[++i];
        else if (!std::strcmp(argv[i],"--frames") && i+1<argc) frames=std::strtoull(argv[++i],nullptr,10);
        else if (!std::strcmp(argv[i],"--screenshot") && i+1<argc) shot=argv[++i];
        else if (!std::strcmp(argv[i],"--bloom") && i+1<argc) bloom=static_cast<f32>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i],"--exposure") && i+1<argc) exposure=static_cast<f32>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i],"--auto-exposure")) autoExposure=true;
        else if (!std::strcmp(argv[i],"--no-vsync")) vsyncOff=true;
        // --lod-select [px]: virtualized-geometry per-instance LOD level selection
        // (aver::trifactor::ClusterAdapt). Optional pixel error budget, default 1.0px.
        else if (!std::strcmp(argv[i],"--lod-select")) {
            lodSelect=true;
            if (i+1 < argc && (argv[i+1][0] != '-' || (argv[i+1][1] >= '0' && argv[i+1][1] <= '9')))
                lodErrorPx=static_cast<f32>(std::atof(argv[++i]));
        }
        // --no-lod-select: draw every instance at LOD 0, whatever the Cook wrote. This is what the
        // editor did by default until the cost of it was measured -- 102.7ms median against 76.7ms
        // with selection on, on the same Electric Dreams camera -- and it is kept only as the escape
        // hatch for telling a selection artefact apart from a real one.
        else if (!std::strcmp(argv[i],"--no-lod-select")) lodSelect=false;
        // --lod-cluster-stats: turns on the informational per-meshlet frustum/cone-cull counters on
        // top of --lod-select. Separate flag on purpose -- see lodClusterStatsEnabled_'s own comment.
        else if (!std::strcmp(argv[i],"--lod-cluster-stats")) lodClusterStats=true;
        // --lod-per-cluster [px]: PER-CLUSTER virtualized-geometry selection (replaces --lod-select's
        // per-level choice for an instance when both are given). Optional pixel error budget, default
        // 1.0px, same knob --lod-select uses.
        else if (!std::strcmp(argv[i],"--lod-per-cluster")) {
            lodPerCluster=true;
            if (i+1 < argc && (argv[i+1][0] != '-' || (argv[i+1][1] >= '0' && argv[i+1][1] <= '9')))
                lodErrorPx=static_cast<f32>(std::atof(argv[++i]));
        }
        // --lod-mesh-shader [px]: the GPU per-cluster path (amplification+mesh shader). Wins over
        // --lod-per-cluster and --lod-select for an instance whose mesh has GPU cluster data AND this
        // device's pipeline came up; falls back per-instance otherwise. Optional pixel error budget,
        // default 1.0px, same knob the other two share.
        else if (!std::strcmp(argv[i],"--lod-mesh-shader")) {
            lodMeshShader=true;
            if (i+1 < argc && (argv[i+1][0] != '-' || (argv[i+1][1] >= '0' && argv[i+1][1] <= '9')))
                lodErrorPx=static_cast<f32>(std::atof(argv[++i]));
        }
        // --no-lod-mesh-shader forces the GPU per-cluster path OFF. It exists because the path is
        // now ON by default wherever the device supports it (see onInit), so "compare against not
        // having it" needs a way to say so -- which is exactly how its 2.8x speedup was measured.
        else if (!std::strcmp(argv[i],"--no-lod-mesh-shader")) lodMeshShader=0;
        else if (!std::strcmp(argv[i],"--ui-demo")) uiDemo=true;
        // Coverage is optional: `--clouds` alone takes the authored default.
        else if (!std::strcmp(argv[i],"--clouds")) {
            clouds=1;
            if (i+1 < argc && argv[i+1][0] != '-') cloudCover=static_cast<f32>(std::atof(argv[++i]));
        }
        // The derived sky, with an optional sun elevation in degrees.
        else if (!std::strcmp(argv[i],"--sky-physical")) {
            skyPhysical=true;
            if (i+1 < argc && (argv[i+1][0] != '-' || (argv[i+1][1] >= '0' && argv[i+1][1] <= '9')))
                skyElevation=static_cast<f32>(std::atof(argv[++i]));
        }
        else if (!std::strcmp(argv[i],"--sky-authored")) skyAuthored=true;
        else if (!std::strcmp(argv[i],"--probe-rel") && i+2<argc) {
            probeU=static_cast<f32>(std::atof(argv[++i]));
            probeV=static_cast<f32>(std::atof(argv[++i]));
        }
        else if (!std::strcmp(argv[i],"--cam") && i+5<argc) {
            camSet=true;
            camX    =static_cast<f32>(std::atof(argv[++i]));
            camY    =static_cast<f32>(std::atof(argv[++i]));
            camZ    =static_cast<f32>(std::atof(argv[++i]));
            camPitch=static_cast<f32>(std::atof(argv[++i]));
            camYaw  =static_cast<f32>(std::atof(argv[++i]));
        }
        else if (!std::strcmp(argv[i],"--tool") && i+1<argc) {
            const char* t=argv[++i];
            tool = !std::strcmp(t,"move")?Tool::Move : !std::strcmp(t,"rotate")?Tool::Rotate :
                   !std::strcmp(t,"scale")?Tool::Scale :
#if AVER_MODULE_LANDSCAPE

#endif
                   Tool::Select;
        }
        else if (argv[i][0]!='-') { if (isOcproject(argv[i])) project=argv[i]; else beam=argv[i]; }
    }
    // Before the engine creates a device: the backend queries the hardware inside Engine::run.
    if (forceCaps && !rhi::setCapsOverride(forceCaps))
        AVER_ERROR("[Sandbox] --force-caps '{}' was rejected; running on the UNCLAMPED device", forceCaps);

    auto* app = new SandboxApp(frames, headless, beam, shot, tool);
    app->setPost(exposure, bloom, autoExposure);
    app->applyCaptureExposureRule(autoExposure);
    app->setGiForceOff(noGi);
    app->setGiConeTraceOff(giConeOff);
    if (clouds) app->setClouds(cloudCover);
    if (skyPhysical) app->setSkyPhysical(skyElevation);
    if (skyAuthored) app->setSkyAuthored();
    app->setVSyncOff(vsyncOff);
    app->setLodSelect(lodSelect, lodErrorPx);
    app->setLodClusterStats(lodClusterStats);
    app->setLodPerCluster(lodPerCluster, lodErrorPx);
    if (lodMeshShader >= 0) app->setLodMeshShader(lodMeshShader != 0, lodErrorPx);
    app->setUiDemo(uiDemo);
    app->setOpenAsset(openAsset);
    app->setGraphTab(graphTab);
    app->setGraphSelectNode(graphSelectNode);
    app->setInputProbe(inputProbe);
    app->setAutoCompile(autoCompile);
    app->setFocusLevelAt(focusLevelAt);
    app->setShowEditorPrefs(showPrefs);
    app->setScrollPrefsToKeybinds(scrollPrefsToKeybinds);
    app->setHudTest(hudTest);
    app->setSaveProject(saveProject);
    app->setImportOnce(importSrc, importDst);
    app->setUseWarp(warp);
    if (!backendName.empty()) app->setBackend(backendName);
    app->setDebugLayer(debugLayer);
    app->setProjectPath(project);
    // The start screen: interactive launches with no project, or --start-screen. Never in a capture run.
    app->armBrowser(startScreen || (!headless && frames == 0 && project.empty()));
    app->setFocusVoxi(focusVoxi);
    if (projectSettingsPage >= 0) app->setProjectSettingsPage(projectSettingsPage);
    app->setDrawerOpen(drawerOpen, drawerSub);
    app->setFocusScript(focusScript);
    app->setFocusTools(focusTools);
    app->setFocusCompileMenu(focusCompileMenu);
    if (!droneGraph.empty()) app->setDroneGraph(droneGraph);
    if (!landscapePath.empty()) app->setLandscapePath(landscapePath);
    // Three states, not two: >0 is an explicit delay, <0 is --no-chunk-stream, and 0 is "the flag was
    // never given" -- which now LEAVES THE MEMBER'S OWN DEFAULT ALONE rather than meaning off. See
    // chunkStreamAutoFrames_'s declaration for why the default is on.
    if (chunkStream > 0)      app->setChunkStreamAuto(chunkStream);
    else if (chunkStream < 0) app->setChunkStreamAuto(0);
    if (fogMatch) app->setFogMatchToStreamRadius(true, fogMatchOpacity);
    if (droneAuto > 0) app->setDroneAuto(droneAuto);
    if (undoTestAuto > 0) app->setUndoTestAuto(undoTestAuto);
    if (keybindTestAuto > 0) app->setKeybindTestAuto(keybindTestMode, keybindTestAuto);
#if AVER_MODULE_MCP
    // Precedence: explicit --mcp <port> (already resolved above) > mcp.conf's editor_bridge.port >
    // the module's own built-in default (45123, McpBridge.hpp's own default arg). mcp.conf ONLY ever
    // supplies the NUMBER used here -- it cannot turn the channel on by itself; --mcp is still
    // required, same opt-in contract as before this file existed. A missing, empty or malformed
    // mcp.conf, or a mcp.conf missing this one key, resolves silently to the built-in default: the
    // editor starts exactly as it always has, and readMcpConfPort has already logged a warning if
    // the reason was a value it could not parse rather than the ordinary "not set" case.
    //
    // The resolved port and its source are logged UNCONDITIONALLY, not only on the fallback path --
    // matching aver_mcp.py's own "always reported, never on request" provenance instinct (see its
    // binary_provenance) -- so an explicit --mcp <port> is just as visible in the log as a value
    // pulled from mcp.conf, and nobody has to infer which one happened from silence.
    if (mcpRequested) {
        const char* source = "command line";
        if (!mcpPortExplicit) {
            constexpr u16 kDefaultMcpPort = 45123;
            u16 confPort = 0;
            if (editor::readMcpConfPort(editor::engineRoot(), "editor_bridge.port", &confPort)) {
                mcpPort = confPort;
                source = "mcp.conf";
            } else {
                mcpPort = kDefaultMcpPort;
                source = "built-in default";
            }
        }
        AVER_INFO("[Mcp] control channel requested on port {} (source: {})", mcpPort, source);
    }
    app->setMcpPort(mcpPort);
#else
    if (mcpRequested) AVER_WARN("[Mcp] --mcp was given but this build has no control channel "
                                "(-DAVER_MODULE_MCP=ON to include it); the editor runs regardless");
#endif
    app->setFocusCompile(focusCompile);
    app->setFocusReload(reloadAt);
    app->setMsaaOverride(msaa);
    app->setGiOverride(gi, giDbg);
    app->setRtOverride(rt);
    app->setRtRays(rtRays);
    app->setRtPixelsPerRay(rtPixelsPerRay);
    app->setRtShadowDenoise(rtShadowDenoise);
    app->setGiUpdateInterval(giUpdateInterval);
    app->setRtForceOff(noRt);
    app->setCamWobble(camWobbleDeg, camWobblePeriod);
    app->setRenderScale(renderScale);
    if (!aversrArg.empty()) {
#if AVER_MODULE_SR
        aver::sr::Quality aversrQuality;
        if (aver::sr::parseQuality(aversrArg.c_str(), aversrQuality)) app->setAverSrQuality(aversrQuality);
        else AVER_ERROR("[AverSR] --aversr '{}' not recognised (off|quality|balanced|performance)", aversrArg);
#else
        AVER_WARN("[AverSR] --aversr '{}' was given but this build has no AverSR module "
                  "(-DAVER_MODULE_SR=ON to include it); the editor renders at native resolution "
                  "regardless", aversrArg);
#endif
    }
    app->setDepthPrepassOverride(depthPrepass);
    if (occlusionCull) {
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
        app->setOcclusionCullOverride(true);
#else
        AVER_WARN("[Occlusion] --occlusion-cull was given but this build has no Occlusion module "
                  "(-DAVER_MODULE_OCCLUSION=ON to include it); every entity draws as it always did");
#endif
    }
    if (edgeAa) {
#if AVER_MODULE_SR
        app->setEdgeAaOverride(true);
#else
        AVER_WARN("[AverSR] --edge-aa was given but this build has no AverSR module "
                  "(-DAVER_MODULE_SR=ON to include it); MSAA (if any) is the only edge AA applied");
#endif
    }
    app->setFrameTimeReport(frameTime);
    app->setMsOverride(ms);
    app->setProbe(probeX, probeY);
    if (probeU >= 0.0f) app->setProbeRel(probeU, probeV);
    if (camSet) app->setCamera(Vec3{camX, camY, camZ}, camPitch, camYaw);
    app->setScriptsDir(scriptsDir);
    app->setSpawnTest(spawnTest);
    if (playTest) app->setPlayTest();
    if (skinTest) app->setSkinTest();
    if (skinDrawTest) app->setSkinDrawTest();
    if (particleTest) app->setParticleTest();
    if (noParticleGi) app->setNoParticleGi();
    if (particleStressEmitters > 0) app->setParticleStress(particleStressEmitters, particleStressMaxParticles);
    if (particleStressSecondEmitter) app->setParticleStressSecondEmitter();
    if (reflTest) app->setReflTest();
    if (furnaceTest) app->setFurnaceTest();
    if (furnaceSun) app->setFurnaceSun();
    if (sunAngle > 0.0f) app->setSunAngle(sunAngle);
    if (ptFurnace) app->setPtFurnaceTest();
    if (ptScene) app->setPtSceneView();
    if (ptSceneToggleOn > 0)  app->setPtSceneToggleOnAuto(ptSceneToggleOn);
    if (ptSceneToggleOff > 0) app->setPtSceneToggleOffAuto(ptSceneToggleOff);
    if (!skinSceneDir.empty()) app->setSkinSceneDir(skinSceneDir);
    return app;
}

} // namespace aver
