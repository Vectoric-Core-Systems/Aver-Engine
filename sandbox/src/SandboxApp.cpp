// SandboxApp: the Aver editor executable. Viewport, gizmos, panels, Content Browser,
// and the frame loop that drives the runtime modules.

#include "aver/runtime/EntryPoint.hpp"
#include "aver/platform/Window.hpp"
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
#if AVER_HAVE_AUDIO_IMPORT
#  include "aver/formats/OcAudio.hpp"
#endif
#include "aver/ui/UiDrawList.hpp"
#include "aver/render/ui/UiRenderer.hpp"
#include "aver/ui/ui_abi.h"

#include "ProjectBrowser.hpp"
#include "ProjectScaffold.hpp"
#include "ToolsMenu.hpp"
#include "UiRegistry.hpp"

#if AVER_MODULE_MCP
#include "aver/mcp/McpBridge.hpp"
#endif
#include "ToolGlyphs.hpp"
#include "AssetEditor.hpp"
#include "ActorEditor.hpp"
#include "EditorPrefs.hpp"
#include "aver/platform/DirectoryWatcher.hpp"
#if AVER_HAVE_ROSLYN
#  include "aver/formats/AverDesign.hpp"
#endif
#include "EngineScaffold.hpp"
#include "IdeIntegration.hpp"
#include "ShellIntegration.hpp"

#if AVER_MODULE_VOXI
#include "aver/voxi/Voxi.hpp"
#include "aver/voxi/VoxiRenderer.hpp"
#endif

#if AVER_MODULE_PBR
#include "aver/pbr/Material.hpp"
#include "aver/pbr/MaterialGpu.hpp"
#include "aver/formats/OcMat.hpp"
#include "aver/formats/MaterialScript.hpp"
#include "aver/assets/TextureUpload.hpp"
#endif

#if AVER_MODULE_SCRIPTING
#include "aver/scripting/ScriptHost.hpp"
#endif

#if AVER_MODULE_FRAMEWORK
#if AVER_MODULE_PHYSICS
#include "aver/physics/physics_abi.h"
#endif
#include "aver/framework/framework_abi.h"
#include "aver/framework/framework_hooks.h"
#endif

#if AVER_MODULE_SCENE
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include "aver/scene/scene_abi.h"
#include "aver/scene/World.hpp"
#include "aver/scene/Components.hpp"
#endif

#if AVER_WITH_IMGUI
#include "imgui.h"
#include "imgui_internal.h"
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
// Builds a quaternion from (roll, pitch, yaw) degrees as Rz * Ry * Rx.
static Quat quatFromEulerDeg(const Vec3& e) {
    return (Quat::fromAxisAngle({0,0,1}, radians(e.z)) * Quat::fromAxisAngle({0,1,0}, radians(e.y)) *
            Quat::fromAxisAngle({1,0,0}, radians(e.x))).normalized();
}
// Exact inverse of quatFromEulerDeg. Returns (roll, pitch, yaw) degrees.
static Vec3 eulerDegFromQuat(const Quat& q) {
    const f32 sinP = 2.0f * (q.w * q.y - q.z * q.x);
    const f32 pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, sinP)));
    f32 roll, yaw;
    if (std::fabs(sinP) > 0.99999f) {
        roll = 0.0f;
        yaw  = std::atan2(-2.0f * (q.x * q.y - q.w * q.z), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
    } else {
        roll = std::atan2(2.0f * (q.w * q.x + q.y * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        yaw  = std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
    }
    const f32 r2d = 180.0f / 3.14159265358979323846f;
    return Vec3{roll * r2d, pitch * r2d, yaw * r2d};
}
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

// The viewport manipulation mode.
enum class Tool { Select, Move, Rotate, Scale };
static const char* kToolNames[4] = {"Select", "Move", "Rotate", "Scale"};

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
        c.enableDebugLayer=debugLayer_; return c;
    }
    void setUseWarp(bool w) { useWarp_ = w; }
    void setDebugLayer(bool d) { debugLayer_ = d; }

#if AVER_WITH_IMGUI
    // Rebuilds the ImGui style and font atlas for the given DPI scale.
    void applyDpi(f32 dpi) {
        dpi_ = dpi;
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

    // Builds the editor: asset editors, MCP, physics, the placeholder scene, gizmos, and the render features.
    void onInit(Engine& e) override {
        AVER_INFO("[Sandbox] backend={} adapter='{}'", rhi::backendName(e.device()->backend()), e.device()->adapterName());

        // Registration order is precedence: the first factory that accepts a path wins.
        assetEditors_.registerFactory(&editor::makeMeshEditor);
        assetEditors_.registerFactory(&editor::makeActorEditor);
        // Must run before any actor factory: the "is Roslyn available" answer is cached on first ask.
        locateAverDesign();
        {
            editor::ActorEditorHooks hooks;
            hooks.compileScripts = [this] { tools_.triggerToolbarCompile(project_); };
            hooks.compileBusy    = [this] { return tools_.compiling(); };
            hooks.drawCompileButton = [this] {
                tools_.drawCompileButton(project_, dpi_, compileIconUiId_);
            };
            hooks.openInIde      = [this](const std::string& p) {
                const editor::IdeInfo& ide = cbIde();
                if (!editor::openInIde(ide, p)) AVER_WARN("[Editor] could not open {} in {}", p, ide.name);
            };
            hooks.ideName = cbIde().name;
            editor::setActorEditorHooks(std::move(hooks));
        }
        window_ = e.window();

#if AVER_MODULE_MCP
        if (mcpPort_) {
            registerMcpAbis();
            mcp_.setWidgetResolver([this](const std::string& n, f32& x, f32& y) {
                return uiReg_.centreOf(n, x, y);
            });
            mcp_.setWidgetLister([this] { return uiReg_.describe(); });
            if (!mcp_.start(mcpPort_))
                AVER_WARN("[Mcp] --mcp was given but the channel did not start; the editor is "
                          "unaffected and carries on");
        }
#endif

        browser_.init();

#if AVER_MODULE_PHYSICS
        // Must start before any level loads: loading builds a static body per colliding placement.
        if (aver_phys_init()) {
            groundBody_ = aver_phys_add_static_box(0.0f, 0.0f, -kGroundHalfThickCm,
                                                   kGroundHalfExtentCm, kGroundHalfExtentCm,
                                                   kGroundHalfThickCm);
            AVER_INFO("[Sandbox] physics started, ground body={} (fixed step {:.4f}s)",
                      groundBody_, aver_phys_fixed_step());
        } else {
            AVER_WARN("[Sandbox] physics failed to start - gameplay will not collide");
        }
#endif
        if (!projectPath_.empty()) {
            std::string err;
            if (browser_.open(projectPath_, &err)) applyProject(e);
            else AVER_WARN("[Sandbox] '{}' not loaded: {}", projectPath_, err);
        }
#if AVER_WITH_IMGUI
        if (e.device()->uiActive()) {
            applyDpi(e.window() ? e.window()->dpiScale() : 1.0f);
            AVER_INFO("[Sandbox] DPI scale {:.2f}, UI font rasterised at {:.0f}px", dpi_, 16.0f * dpi_);
            if (browserActive_) loadLogo(e);
            loadCompileIcon(e);
            loadIconSheet(e, "file-icons.png",   4, "FileTypeIcons", fileIconsTexture_,   fileIconsUiId_,   fileIconAspect_);
            loadIconSheet(e, "folder-icons.png", 2, "FolderIcons",   folderIconsTexture_, folderIconsUiId_, folderIconAspect_);
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
        {
            std::vector<rhi::MeshVertex> sv; std::vector<u32> si;
            appendSphere(sv, si, 1.0f, 24, 48);
            sceneMeshes_[fnv1a64(std::string_view("Meshes/sphere.ocmesh"))] =
                e.device()->createMesh(sv.data(), (u32)sv.size(), si.data(), (u32)si.size());
            sceneMeshes_[fnv1a64(std::string_view("Meshes/cube.ocmesh"))] = unitCube;
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

#if AVER_MODULE_VOXI
        // Hand the GPU's real capabilities to Voxi so its settings reflect this hardware.
        {
            const rhi::DeviceCaps c = e.device()->caps();
            voxi::DeviceInfo di;
            di.msaaMask = c.msaaMask; di.maxMsaaSamples = c.maxMsaaSamples;
            di.rayTracingTier = c.rayTracingTier; di.computeShaders = c.computeShaders;
            di.typedUavLoads = c.typedUavLoads; di.conservativeRaster = c.conservativeRaster;
            di.shaderModel = c.shaderModel; di.meshShaderTier = c.meshShaderTier;
            di.dxcAvailable = c.dxcAvailable;
            voxi::Renderer::get().setDeviceInfo(di);
            voxi::Settings s = voxi::Renderer::get().settings();
            s.msaa = static_cast<voxi::Msaa>(e.device()->sampleCount());
            if (msaaOverride_) s.msaa = static_cast<voxi::Msaa>(msaaOverride_);
            // --no-gi wins over --gi.
            if (giForceOff_)     s.globalIllumination = voxi::Quality::Off;
            else if (giOverride_) s.globalIllumination = static_cast<voxi::Quality>(giOverride_);
            if (rtOverride_) s.rayTracing = static_cast<voxi::Quality>(rtOverride_);
            if (msOverride_) s.meshShaders = true;
            voxi::Renderer::get().setSettings(s);
            AVER_INFO("[Voxi] attached: MSAA {}x, RT tier {}, SM {}, mesh tier {}", c.maxMsaaSamples, c.rayTracingTier, c.shaderModel, c.meshShaderTier);

            // Registration is non-owning: voxiRenderer_ must outlive the device, torn down in onShutdown.
            voxiRenderer_.setSettings(s);
            if (voxiRenderer_.init(*e.device())) {
                e.device()->addRenderFeature(&voxiRenderer_);
                voxiAttached_ = true;
                if (projectRenderPending_) applyProjectRenderSettings();
                if (saveProject_ && !saveProjectDone_) { saveProjectDone_ = true; seedAndSaveProject(); }
                if (!importSrc_.empty() && !importDone_) {
                    importDone_ = true;
                    importAsset(importSrc_, importDst_);
                }
#if AVER_MODULE_PBR
                textureFactory_ = e.device()->resources();
                voxiRenderer_.materials().setTextureResolver(&SandboxApp::resolveMaterialTexture, this);
#endif
            }
        }
#endif
        // The game UI's render feature: overlay pass only, so ordering against scene features is free.
        gameUi_ = aver::render::ui::UiRenderer::create(*e.device());
        if (gameUi_) e.device()->addRenderFeature(gameUi_);
#if AVER_MODULE_SCRIPTING
        {
            scripting::HostDesc hd;
            hd.bridgeDir = executableDir() + "\\Scripting";
            hd.scriptsDir = resolveScriptsDir();
            scripts_.init(hd);

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
    }

    // Advances one frame: MCP commands, camera, gameplay tick, physics, and the render state.
    void onUpdate(Engine& e, const Timestep& t) override {
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
            if (levelFocused_ && !io.WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_F) && anySelected()) {
                EditXform x;
                if (selectedXform(x)) {
                    const f32 r = selectedRadius();
                    const f32 d = std::fmax(50.0f, r / std::tan(radians(30.0f)) * 1.6f);
                    camPos_ = x.pos - fwd * d;
                    flySpeed_ = std::fmax(flySpeed_, r * 0.4f);
                }
            }
        }
#endif
#if AVER_MODULE_SCRIPTING
        scripts_.update(t.dt);
#endif
#if AVER_MODULE_FRAMEWORK
        const bool interactive = maxFrames_ == 0 && !playTest_;
        if (e.device()->uiActive() && interactive) {
            // Clicking the viewport puts the mouse back in the game. Tested before wantCapture below.
            {
                const ImGuiIO& mio = ImGui::GetIO();
                if (playSessionActive() && releasedByUser_ && ImGui::IsMouseClicked(0) &&
                    !mio.WantCaptureMouse && inViewport(mio.MousePos.x, mio.MousePos.y))
                    releasedByUser_ = false;
            }
            const bool wantCapture = playSessionActive() && !releasedByUser_;
            if (ImGui::IsKeyPressed(ImGuiKey_F1, false) && ImGui::GetIO().KeyShift && playSessionActive())
                releasedByUser_ = !releasedByUser_;
            if (!playSessionActive()) releasedByUser_ = false;
            setMouseCaptured(wantCapture && !ImGui::GetIO().WantTextInput);
        }
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
            const Vec3 sd = Vec3{sky_.sunDirection[0], sky_.sunDirection[1],
                                 sky_.sunDirection[2]}.getSafeNormal();
            voxiRenderer_.setSun(&sd.x, sunColor_, sunAmbient_);
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
    }

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
            projectMeshIds_.push_back(id);
            ++loaded;
            AVER_INFO("[Mesh] '{}' -> {} verts, {} indices", rel, verts.size(), md.indices.size());
        }
        if (loaded || failed)
            AVER_INFO("[Mesh] {} project mesh(es) loaded from {}{}", loaded, dir,
                      failed ? (", " + std::to_string(failed) + " failed") : "");
#else
        (void)e;
#endif
    }

    // Drops the project's meshes from the id table. The built-in primitives survive.
    void releaseProjectMeshes() {
        for (const u64 id : projectMeshIds_) sceneMeshes_.erase(id);
        projectMeshIds_.clear();
    }

    // Loads every .ocmat under Content/Materials and binds each to the surface token its stem interns to.
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
        hideEditorScene_ = playSessionActive() || !levelEntities_.empty();
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

#if AVER_MODULE_SCENE
        // Scene-entity pass: draws every live entity carrying a CMeshRenderer.
        {
            scene::World& w = scene::World::instance();
            int drawn = 0;
            const u32 n = w.count();
            for (u32 i = 0; i < n; ++i) {
                const scene::Entity ent = w.at(i);
                if (w.destroyPending(ent)) continue;
                const scene::CMeshRenderer* mr =
                    w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
                if (!mr || !(mr->flags & scene::kMeshRendererVisible) || mr->mesh == 0) continue;
                const auto it = sceneMeshes_.find(mr->mesh);
                if (it == sceneMeshes_.end()) continue;
                const Mat4& wm = w.worldMatrix(ent);
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
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
                if (pbr::MaterialSystem& ms = voxiRenderer_.materials(); ms.ready())
                    e.device()->setDrawBinding(ms.bindingSet(authored), &ms.constants(authored),
                                               sizeof(pbr::MaterialConstants));
#endif
                e.device()->drawMesh(it->second, &wm.m[0][0], col, metallic, roughness);
                if (sel_ == kSelScene && ent == selEntity_)
                    selectionOutline_ = wm, selectionMesh_ = it->second, hasSelection_ = true;
                ++drawn;
            }
            if (drawn != lastSceneDrawn_) {
                AVER_INFO("[Sandbox] scene-render: {} spawned CMeshRenderer entit{} drawn",
                          drawn, drawn == 1 ? "y" : "ies");
                lastSceneDrawn_ = drawn;
            }
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
        buildUI(e);
        uiReg_.endFrame();
        submitGameUi(e);
        captureCheck(e);
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
        groundBody_ = 0;
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
        if (gameUi_) {
            e.device()->removeRenderFeature(gameUi_);
            delete gameUi_;
            gameUi_ = nullptr;
        }
#if AVER_MODULE_VOXI
        if (voxiAttached_) { e.device()->removeRenderFeature(&voxiRenderer_); voxiAttached_ = false; }
        voxiRenderer_.shutdown();
#else
        (void)e;
#endif
#if AVER_MODULE_SCRIPTING
        scripts_.shutdown();
#endif
        AVER_INFO("[Sandbox] shutdown");
    }
    void setVSyncOff(bool off) { vsyncOffRequested_ = off; }               // --no-vsync
    void setUiDemo(bool on) { showUiDemo_ = on; }                          // --ui-demo
    void setOpenAsset(std::string p) { openAsset_ = std::move(p); }        // --open-asset
    void setInputProbe(bool on) { inputProbe_ = on; }
    void setAutoCompile(bool on) { autoCompile_ = on; }   // --auto-compile, and the Tools menu
    void setFocusLevelAt(int frame) { focusLevelAt_ = frame; }   // --focus-level-at <N>
    void setShowEditorPrefs(bool on) { if (on) showEditorPrefs_ = true; }   // --editor-prefs
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
    void setFocusCompile(bool b) { tools_.armCompile(b); }   // --compile-scripts
    void setFocusReload(int frames) { if (frames > 0) tools_.armReload(frames); } // --reload-scripts [N]
    void setMsaaOverride(int n) { msaaOverride_ = n; }   // --msaa N
    void setGiOverride(int q, bool dbg) { giOverride_ = q; giDebugView_ = dbg; } // --gi / --gi-debug
    void setGiForceOff(bool off) { giForceOff_ = off; }                        // --no-gi
    void setRtOverride(int q) { rtOverride_ = q; }                              // --rt
    void setMsOverride(bool on) { msOverride_ = on; }                           // --ms
    void setProbe(u32 x, u32 y) { probeX_ = x; probeY_ = y; }                    // --probe X Y
    void setProbeRel(f32 u, f32 v) { probeU_ = u; probeV_ = v; }                 // --probe-rel U V
    void setScriptsDir(std::string d) { scriptsDir_ = std::move(d); }            // --scripts <dir>
    void setSpawnTest(std::string cls) { spawnTestClass_ = std::move(cls); }      // --spawn-test <ClassName>
    void setPlayTest() { playTest_ = true; }                                       // --play-test
    void setProjectPath(std::string p) { projectPath_ = std::move(p); }          // <path>.ocproject
    void armBrowser(bool on) { browserActive_ = on; }   // shows the start screen

private:
    // Adopts the project the browser or command line loaded, and refreshes everything keyed to it.
    void applyProject(Engine& e) {
        project_ = browser_.project();
        editor::setActorEditorContentRoot(project_.contentDir());
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
        releaseProjectMaterials();
        rebuildContentIndex();
        loadProjectMaterials();
#endif
#if AVER_MODULE_SCENE
        releaseProjectMeshes();
        loadProjectMeshes(e);
#endif
#if AVER_MODULE_SCENE
        loadStartMap();
#endif
#if AVER_MODULE_SCRIPTING
        if (scripts_.ready() && scriptsDir_.empty() && project_.valid()) {
            const std::string bin = editor::scriptsBinaryDir(project_);
            const i32 n = scripts_.loadScripts(bin);
            if (n > 0) AVER_INFO("[Scripting] {} project behaviour(s) live from {}", n, bin);
            else AVER_INFO("[Scripting] no built scripts in {} - use Tools > Reload Scripts", bin);
        }
#endif
    }

#if AVER_MODULE_SCRIPTING
    // Tells the formats layer where averdesign.exe is installed.
    void locateAverDesign() const {
#if AVER_HAVE_ROSLYN
        fmt::setAverDesignPath(executableDir() + "/Tools/averdesign.exe");
#endif
    }

    // Returns the directory the CLR host loads user assemblies from: --scripts, else the project's
    // Binaries\Scripts, else <exe>\Scripts.
    std::string resolveScriptsDir() const {
        if (scriptsDir_.empty())
            return project_.valid() ? editor::scriptsBinaryDir(project_) : executableDir() + "\\Scripts";
        const std::string& sd = scriptsDir_;
        const bool absolute = sd.size() > 1 && (sd[1] == ':' || sd[0] == '\\' || sd[0] == '/');
        return absolute ? sd : executableDir() + "\\" + sd;
    }

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
        vx.setSettings(s);   // clamps to this device; the manifest keeps what was asked for
        voxiRenderer_.setSettings(vx.settings());
        AVER_INFO("[Project] applied render settings from {}", project_.manifestPath);
    }

    // Copies the controls' requested values into the manifest struct, before the renderer clamps them.
    void captureRenderSettingsFromUi(const voxi::Settings& requested) {
        project_.giQuality       = static_cast<int>(requested.globalIllumination);
        project_.rayTracing      = static_cast<int>(requested.rayTracing);
        project_.pathTracing     = static_cast<int>(requested.pathTracing);
        project_.voxelResolution = static_cast<int>(requested.voxelResolution);
        project_.giIntensity     = requested.giIntensity;
        project_.giMaxDistance   = requested.giMaxDistance;
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
        if (project_.voxelResolution <= 0)   project_.voxelResolution = static_cast<int>(d.voxelResolution);
        if (project_.giIntensity     < 0.0f) project_.giIntensity     = d.giIntensity;
        if (project_.giMaxDistance   < 0.0f) project_.giMaxDistance   = d.giMaxDistance;
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
            if (autoCompile_) scheduleAutoCompile("the watcher lost records");
            return;
        }
        for (const FileEvent& ev : watchEvents_) {
            if (ev.kind == FileChange::Deleted) continue;
            const std::string full = (std::filesystem::path(contentWatch_.root()) / ev.path).string();
            if (assetEditors_.notifyFileChanged(full))
                AVER_TRACE("[Editor] '{}' changed on disk; its tab was told", ev.path);
            if (autoCompile_ && isScriptSource(ev.path)) scheduleAutoCompile(ev.path);
        }
        serviceAutoCompile();
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
    static constexpr int kAutoCompileQuietMs = 500;
    std::chrono::steady_clock::time_point autoCompileDue_{};
    int autoCompilePending_ = 0;
    std::string autoCompileReason_;

    // Unloads the live script assemblies and loads the ones in binDir. Writes a status line.
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
    void startPlay() {
        const int32_t gm = aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_MODE);
        if (gm == 0) {
            AVER_WARN("[Sandbox] Play: no GameMode class is loaded - open a project with scripts (--scripts <dir>)");
            return;
        }
        const int32_t gi = aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_INSTANCE);  // 0 == none, allowed
        if (aver_fw_begin_play(gi, gm))
            AVER_INFO("[Sandbox] Play: begin_play GameMode='{}'{}", aver_fw_class_name(gm),
                      gi ? std::string(" GameInstance='") + aver_fw_class_name(gi) + "'" : std::string());
        else
            AVER_WARN("[Sandbox] Play: begin_play was rejected (already playing?)");
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
        const bool chordSpace = io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false);
        const bool chordEsc   = drawer_ != Drawer::None && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
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
        if (aver_fw_play_state() != AVER_FW_PLAY_PLAYING) return;
        const int32_t pawn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
        if (pawn == 0) return;
        const scene::Entity e = static_cast<scene::Entity>(static_cast<uint32_t>(pawn));
        scene::World& w = scene::World::instance();
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

    // One undoable edit: a transform change, a create, or a destroy, with everything needed to
    // rebuild a destroyed scene entity held by value.
    struct EditCmd {
        enum class Kind { Transform, Create, Destroy };
        Kind kind = Kind::Transform;
        EditId id = 0;            // a scene entity, through the indirection
        int objIndex = -1;        // or an objects_ index, for the placeholder scene
        EditXform before{}, after{};
        std::string asset, label;
        u64 meshId = 0;
        i32 material = 0;
        bool hadBody = false;
        Vec3 bodyHalf{0,0,0};
    };

    // Returns the edit id bound to an entity, minting one on first use.
    EditId editIdFor(scene::Entity e) {
        const u32 key = static_cast<u32>(e);
        if (const auto it = entityToEdit_.find(key); it != entityToEdit_.end()) return it->second;
        const EditId id = nextEditId_++;
        entityToEdit_[key] = id;
        editToEntity_[id]  = e;
        return id;
    }
    // Returns the entity an edit id names, or kInvalidEntity.
    scene::Entity entityForEdit(EditId id) const {
        const auto it = editToEntity_.find(id);
        return it == editToEntity_.end() ? scene::kInvalidEntity : it->second;
    }
    // Points an existing edit id at a newly created entity.
    void rebindEdit(EditId id, scene::Entity e) {
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
            sel_ = c.objIndex; selEntity_ = scene::kInvalidEntity;
        }
    }

#if AVER_MODULE_SCENE
    // Describes a live entity fully enough to rebuild it after a destroy.
    EditCmd describeEntity(scene::Entity e) {
        scene::World& w = scene::World::instance();
        EditCmd c;
        c.id = editIdFor(e);
        c.asset = w.name(e);
        if (const auto it = entityLabels_.find(static_cast<u32>(e)); it != entityLabels_.end()) c.label = it->second;
        if (const auto* loc = w.component<scene::CLocal>(e, scene::kComponentLocal)) {
            c.after.pos = loc->xf.position;
            c.after.rotDeg = eulerDegFromQuat(loc->xf.rotation);
            c.after.scale = loc->xf.scale;
        }
        if (const auto* mr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer)) {
            c.meshId = mr->mesh; c.material = mr->material;
        }
#if AVER_MODULE_PHYSICS
        if (const auto it = entityBodies_.find(static_cast<u32>(e)); it != entityBodies_.end()) {
            c.hadBody = true;
            c.bodyHalf = c.after.scale;
        }
#endif
        return c;
    }

    // Rebuilds an entity a command destroyed and rebinds its EditId to the new handle.
    void recreateFrom(const EditCmd& c) {
        scene::World& w = scene::World::instance();
        Transform xf; xf.position = c.after.pos; xf.rotation = quatFromEulerDeg(c.after.rotDeg); xf.scale = c.after.scale;
        const scene::Entity e = w.create(c.asset, scene::kInvalidEntity, xf);
        if (e == scene::kInvalidEntity) { AVER_WARN("[Editor] undo: the world refused to recreate '{}'", c.asset); return; }
        if (auto* mr = static_cast<scene::CMeshRenderer*>(w.addComponent(e, scene::kComponentMeshRenderer))) {
            mr->mesh = c.meshId; mr->material = c.material;
            mr->flags |= scene::kMeshRendererVisible;
            mr->aabbMin[0] = mr->aabbMin[1] = mr->aabbMin[2] = -1.0f;
            mr->aabbMax[0] = mr->aabbMax[1] = mr->aabbMax[2] =  1.0f;
        }
        levelEntities_.push_back(e);
        if (!c.label.empty()) entityLabels_[static_cast<u32>(e)] = c.label;
#if AVER_MODULE_PHYSICS
        if (c.hadBody && aver_phys_ready()) {
            const int32_t body = aver_phys_add_static_box(xf.position.x, xf.position.y, xf.position.z,
                                                          c.bodyHalf.x, c.bodyHalf.y, c.bodyHalf.z);
            levelBodies_.push_back(body);
            entityBodies_[static_cast<u32>(e)] = body;
        }
#endif
        rebindEdit(c.id, e);
        sel_ = kSelScene; selEntity_ = e;
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
    void undo() {
        if (undoStack_.empty()) return;
        EditCmd c = undoStack_.back(); undoStack_.pop_back();
        switch (c.kind) {
            case EditCmd::Kind::Transform: applyXformTo(c, c.before); break;
#if AVER_MODULE_SCENE
            case EditCmd::Kind::Create:    destroyEntity(entityForEdit(c.id));
                                           sel_ = -1; selEntity_ = scene::kInvalidEntity; break;
            case EditCmd::Kind::Destroy:   recreateFrom(c); break;
#else
            default: break;
#endif
        }
        redoStack_.push_back(std::move(c));
    }

    // Re-applies the newest undone command and moves it back to the undo stack.
    void redo() {
        if (redoStack_.empty()) return;
        EditCmd c = redoStack_.back(); redoStack_.pop_back();
        switch (c.kind) {
            case EditCmd::Kind::Transform: applyXformTo(c, c.after); break;
#if AVER_MODULE_SCENE
            case EditCmd::Kind::Create:    recreateFrom(c); break;
            case EditCmd::Kind::Destroy:   destroyEntity(entityForEdit(c.id));
                                           sel_ = -1; selEntity_ = scene::kInvalidEntity; break;
#else
            default: break;
#endif
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
        sel_ = (int)objects_.size() - 1; selEntity_ = scene::kInvalidEntity;
    }
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

    // Returns which gizmo handle is under the cursor: 0..2 axis, 3 = centre, -1 = none.
    int pickAxis(const Vec3& origin, f32 L, f32 mx, f32 my) const {
        f32 ox, oy; if (!project(origin, ox, oy)) return -1;
        const f32 thr = 16.0f * dpi_;
        if (tool_ == Tool::Rotate) {
            int best=-1; f32 bestD=thr;
            for (int a=0;a<3;++a) {
                const Vec3 P=kAxisDir[(a+1)%3], Q=kAxisDir[(a+2)%3];
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
            f32 tx, ty; if (!project(origin + kAxisDir[a]*L, tx, ty)) continue;
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
            const Vec3 A = kAxisDir[activeAxis_];
            f32 s0x,s0y,s1x,s1y;
            if (project(o.pos, s0x, s0y) && project(o.pos + A, s1x, s1y)) {
                const f32 px=s1x-s0x, py=s1y-s0y, pl2=px*px+py*py;
                if (pl2 > 1e-4f) o.pos += A * ((dx*px + dy*py) / pl2);
            }
        }
        if (snapMove_) for (int k=0;k<3;++k) (&o.pos.x)[k] = snapf((&o.pos.x)[k], moveSnap_);
    }
    // Scales the transform by a mouse delta in pixels, along the active axis or uniformly.
    void applyScale(EditXform& o, f32 dx, f32 dy) {
        auto bump = [&](int a, f32 amt){ f32& c=(&o.scale.x)[a]; c += amt; if (c<0.02f) c=0.02f; };
        if (activeAxis_ == 3) { const f32 amt=(dx - dy)/80.0f; for (int a=0;a<3;++a) bump(a, amt); }
        else {
            const Vec3 A = kAxisDir[activeAxis_];
            f32 s0x,s0y,s1x,s1y;
            if (project(o.pos, s0x, s0y) && project(o.pos + A, s1x, s1y)) {
                const f32 px=s1x-s0x, py=s1y-s0y, pl=std::sqrt(px*px+py*py);
                if (pl > 1e-3f) bump(activeAxis_, ((dx*px + dy*py)/pl) / 60.0f);
            }
        }
        if (snapScale_) for (int k=0;k<3;++k) (&o.scale.x)[k] = std::fmax(0.02f, snapf((&o.scale.x)[k], scaleSnap_));
    }
    // Rotates the transform about the active axis by the angle the cursor swept around the ring.
    void applyRotate(EditXform& o, f32 px, f32 py, f32 mx, f32 my) {
        f32 ox, oy; if (!project(o.pos, ox, oy)) return;
        const f32 a0=std::atan2(py-oy, px-ox), a1=std::atan2(my-oy, mx-ox);
        f32 da=a1-a0; while (da> kPi) da-=kTwoPi; while (da< -kPi) da+=kTwoPi;
        const f32 s = dot(kAxisDir[activeAxis_], camForward()) >= 0.0f ? -1.0f : 1.0f;
        f32& comp = (&o.rotDeg.x)[activeAxis_];
        comp += degrees(da) * s;
        if (snapRot_) comp = snapf(comp, rotSnap_);
    }

    // Runs the tool keys, picking, and the gizmo drag for one frame.
    void handleManip(Engine& e) {
#if AVER_WITH_IMGUI
        if (!e.device()->uiActive() || browserActive_) return;
        if (gameHasInput()) return;
        const ImGuiIO& io = ImGui::GetIO();

        if (levelFocused_ && !io.WantCaptureKeyboard) {
            if (ImGui::IsKeyPressed(ImGuiKey_1)) tool_=Tool::Select;
            if (ImGui::IsKeyPressed(ImGuiKey_2)) tool_=Tool::Move;
            if (ImGui::IsKeyPressed(ImGuiKey_3)) tool_=Tool::Rotate;
            if (ImGui::IsKeyPressed(ImGuiKey_4)) tool_=Tool::Scale;
        }
        const f32 mx=io.MousePos.x, my=io.MousePos.y;
        const bool overScene = levelHovered_ && inViewport(mx, my);

        hoverAxis_ = -1;
        EditXform gx;
        const bool haveGizmo = tool_!=Tool::Select && anySelected() && selectedXform(gx);
        if (haveGizmo && !dragging_ && overScene)
            hoverAxis_ = pickAxis(gx.pos, gizmoLen(gx.pos), mx, my);

        if (ImGui::IsMouseClicked(0) && overScene) {
            int ax = -1;
            if (haveGizmo)
                ax = pickAxis(gx.pos, gizmoLen(gx.pos), mx, my);
            if (ax >= 0) {
                dragging_=true; activeAxis_=ax; prevMouseX_=mx; prevMouseY_=my;
                beginTransformEdit();
            }
            else pick(e, io);
        }
        if (!io.MouseDown[0]) {
            if (dragging_) endTransformEdit();
            dragging_=false; activeAxis_=-1;
        }

        if (levelFocused_ && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
            deleteSelection();

        if (levelFocused_ && io.KeyCtrl && !io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) redo(); else undo(); }
            if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
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

    // Draws the current tool's gizmo over the selection, on top of geometry.
    void drawGizmo(Engine& e) {
        if (tool_==Tool::Select) return;
        EditXform x;
        if (!selectedXform(x)) return;
        const Vec3 O = x.pos;
        const f32 L = gizmoLen(O);
        const Mat4 w = Mat4::scale(Vec3{L,L,L}) * Mat4::translation(O);
        const rhi::LineHandle* nrm = tool_==Tool::Move ? gzMove_ : tool_==Tool::Rotate ? gzRot_ : gzScale_;
        const rhi::LineHandle* hi  = tool_==Tool::Move ? gzMoveHi_ : tool_==Tool::Rotate ? gzRotHi_ : gzScaleHi_;
        e.device()->setLineDepth(false);
        for (int a=0;a<3;++a) {
            const bool active = (dragging_ && a==activeAxis_) || (!dragging_ && a==hoverAxis_);
            e.device()->drawLines(active ? hi[a] : nrm[a], &w.m[0][0]);
        }
        e.device()->setLineDepth(true);
    }

#if AVER_WITH_IMGUI
    // Removes the selected entity or placeholder object from the world. Pseudo-entries are ignored.
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
            objects_.erase(objects_.begin() + sel_);
            sel_ = -1;
        }
    }

    // Selects whatever the cursor's ray hits first, across both the placeholder and scene worlds.
    void pick(Engine& e, const ImGuiIO& io) {
        (void)e;
        const f32 nx = (io.MousePos.x - vpX_) / vpW_ * 2.f - 1.f;   // NDC within the viewport rect
        const f32 ny = 1.f - (io.MousePos.y - vpY_) / vpH_ * 2.f;
        const Mat4& iv = invVP_;
        const f32 rx = nx*iv.m[0][0]+ny*iv.m[1][0]+iv.m[2][0]+iv.m[3][0];
        const f32 ry = nx*iv.m[0][1]+ny*iv.m[1][1]+iv.m[2][1]+iv.m[3][1];
        const f32 rz = nx*iv.m[0][2]+ny*iv.m[1][2]+iv.m[2][2]+iv.m[3][2];
        const f32 rw = nx*iv.m[0][3]+ny*iv.m[1][3]+iv.m[2][3]+iv.m[3][3];
        const Vec3 farW{rx/rw, ry/rw, rz/rw};
        const Vec3 ro = eye_, rd = farW - eye_;
        int best=-1; f32 bestT=1e30f;
        if (!hideEditorScene_)
            for (int i=0;i<(int)objects_.size();++i){
                MeshObj& o=objects_[i]; if(!o.visible) continue;
                Transform tr; tr.position=o.pos; tr.rotation=quatFromEulerDeg(o.rotDeg); tr.scale=o.scale;
                const Mat4 iw = tr.toMatrix().inverse();
                const Vec3 lo=xformPoint(iw,ro), ld=xformVec(iw,rd);
                f32 t; if (rayAabb(lo,ld,o.aabbMin,o.aabbMax,t) && t<bestT){ bestT=t; best=i; }
            }

        scene::Entity bestEnt = scene::kInvalidEntity;
#if AVER_MODULE_SCENE
        {
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
                f32 t; if (rayAabb(lo, ld, lmin, lmax, t) && t < bestT) { bestT = t; bestEnt = ent; best = -1; }
            }
        }
#endif
        if (bestEnt != scene::kInvalidEntity) { sel_ = kSelScene; selEntity_ = bestEnt; }
        else                                  { sel_ = best;     selEntity_ = scene::kInvalidEntity; }
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
                upgradeStatus_ = "Project upgraded - Compile C# to rebuild.";
                AVER_INFO("[Editor] '{}' upgraded", project_.name);
            } else {
                upgradeStatus_ = "Upgrade failed: " + err;
                AVER_ERROR("[Editor] upgrade of '{}' failed: {}", project_.name, err);
            }
            pendingUpgrade_ = {};
            upgradeAsked_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Not now", ImVec2(120.0f * dpi_, 0.0f))) {
            upgradeAsked_ = true;
            upgradeStatus_ = "Project left as it is.";
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Not now leaves every file untouched.");
        ImGui::EndPopup();
#endif
    }

    // Builds the whole editor UI for one frame: menu bar, toolbars, panels, drawers and dialogs.
    void buildUI(Engine& e) {
        uiReg_.beginFrame();
        prefsDevice_ = e.device();
        if (!prefsLoaded_) { prefsLoaded_ = true; loadEditorPreferences(); }
#if AVER_MODULE_SCENE
        if (wantMeshReload_) {
            wantMeshReload_ = false;
            releaseProjectMeshes();
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
                case editor::BrowserAction::Quit: e.requestExit(); break;
                case editor::BrowserAction::Stay: break;
            }
            return;
        }

        // Drawer shortcuts: Ctrl+Space toggles the Content Browser, Escape closes an open drawer.
        {
            const ImGuiIO& io = ImGui::GetIO();
            if (!io.WantTextInput && !io.WantCaptureKeyboard) {
                if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false)) toggleDrawer(Drawer::Content);
                if (drawer_ != Drawer::None && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
                    ImGui::IsKeyPressed(ImGuiKey_Escape, false))
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
                if (ImGui::MenuItem("New Level")) { unloadLevel(); levelName_ = "untitled"; }
                uiReg_.track("file.newLevel");
                if (ImGui::MenuItem("Open Level")) loadStartMap();
                uiReg_.track("file.openLevel");
                if (ImGui::MenuItem("Save Level", "Ctrl+S") && !levelPath_.empty()) saveLevel(levelPath_);
                uiReg_.track("file.saveLevel");
                ImGui::EndDisabled();
                if (!haveProject && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Open or create a project first - a level belongs to one.");
#else
                ImGui::MenuItem("New Level"); ImGui::MenuItem("Open Level..."); ImGui::MenuItem("Save Level");
#endif
                ImGui::Separator(); if(ImGui::MenuItem("Exit")) e.requestExit(); ImGui::EndMenu(); }
                        const bool open_edit = ImGui::BeginMenu("Edit");
            uiReg_.track("menu.edit");
            if (open_edit){
                if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo())) undo();
                uiReg_.track("edit.undo");
                if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo())) redo();
                uiReg_.track("edit.redo");
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
                uiReg_.track("window.outputLog");
                ImGui::Separator();
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
        ImGui::BeginDisabled(levelPath_.empty());
        if (ImGui::Button("Save")) saveLevel(levelPath_);
        uiReg_.track("toolbar.save");
        ImGui::EndDisabled();
        if (levelPath_.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("No level loaded - File > New Level, then Save Level As");
        ImGui::SameLine();
        if (dropButton("Add")) ImGui::OpenPopup("addActor");
        uiReg_.track("toolbar.add");
        if (ImGui::BeginPopup("addActor")) {
            ImGui::TextDisabled("Place Actor"); ImGui::Separator();
            if (ImGui::Selectable("Cube"))     spawnCube(e);
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
            ImGui::BeginDisabled(playing);
            if (ImGui::Button("Play")) startPlay();
            uiReg_.track("toolbar.play");
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!playing);
            if (ImGui::Button(ps == AVER_FW_PLAY_PAUSED ? "Resume" : "Pause"))
                aver_fw_set_paused(ps != AVER_FW_PLAY_PAUSED ? 1 : 0);
            ImGui::SameLine();
            if (ImGui::Button("Stop")) { aver_fw_end_play(); AVER_INFO("[Sandbox] Stop: play session ended"); }
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
        if (!openAsset_.empty() && frameNo_ > 5) {
            const std::string want = openAsset_;
            openAsset_.clear();
            if (assetEditors_.open(want)) AVER_INFO("[Editor] --open-asset opened {}", want);
            else AVER_ERROR("[Editor] --open-asset: no registered editor accepts {}", want);
        }
        pumpContentWatch();
        assetEditors_.draw(e, centralDock_, dpi_);
        tools_.drawModals(project_, dpi_);
        drawUpgradePrompt();

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
        ImGui::BeginDisabled(!cbIsEditable(cbSelectedDir_));
        if (ImGui::Button("Import...")) cbWantImport_ = true;
        ImGui::EndDisabled();
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
            const bool editable = cbIsEditable(cbSelectedDir_);
            ImGui::BeginDisabled(!editable);
            if (ImGui::MenuItem("New Folder")) { cbWantNewFolder_ = true; cbNewFolderBuf_[0] = '\0'; }
            if (ImGui::MenuItem("Import...")) cbWantImport_ = true;
            ImGui::EndDisabled();
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
        if (cbWantImport_)    { ImGui::OpenPopup("cbImport");    cbWantImport_ = false; }

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

    // Returns a file's sprite tile: 0 C# Script, 1 C# Class, 2 C++ Class, 3 C++ Module, -1 none.
    // A .cs is classified by peeking at its head and cached against the file's modification time.
    int fileIconTile(const std::string& path, const std::string& name, const std::string& ext) {
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
        if (tile >= 0 && fileIconsUiId_) { blitTile(dl, fileIconsUiId_, centre, s, fileIconAspect_, tile, 4); return; }
        fileGlyph(dl, centre, s, IM_COL32(150, 154, 162, 255));
    }

    // Returns a path's extension, lower-cased.
    static std::string lowerExt(const std::filesystem::path& p) {
        std::string ext = p.extension().string();
        for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return ext;
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
                cbItemContextMenu(e.full, e.name, e.isDir);
                ImGui::PopID();
            }
        }
        clipper.End();
    }

    // Draws the Import modal: a source path and the destination folder.
    void drawImportModal() {
        if (!ImGui::BeginPopup("cbImport")) return;
        ImGui::TextUnformatted("Import an asset into the selected folder.");
        ImGui::TextDisabled(".gltf/.glb become .ocmesh; .wav/.mp3/.m4a/.flac become .ocaudio.");
        ImGui::TextDisabled("Anything else is copied as-is.");
        ImGui::SetNextItemWidth(420.0f * dpi_);
        ImGui::InputText("Source file", importPath_, sizeof(importPath_));
        const std::string dest = cbSelectedDir_.empty() ? project_.contentDir() : cbSelectedDir_;
        ImGui::TextDisabled("Into: %s", dest.c_str());
        ImGui::BeginDisabled(importPath_[0] == '\0');
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

        if (written == 0) { cbStatus_ = "Import produced nothing - see the Output Log"; return; }
        cbStatus_ = "Imported " + std::to_string(written) + " mesh(es) from " +
                    std::filesystem::path(src).filename().string();
        cbInvalidate(destDir);
        wantMeshReload_ = true;
    }

    // Writes an edited material back to the .cs under Content\Materials that declares it, found by
    // trying each in turn. Returns the file written, or "" with err set.
    std::string saveMaterialSource(const std::string& name, const pbr::MaterialDesc& d, std::string& err) {
#if AVER_MODULE_PBR
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
#else
        (void)name; (void)d; err = "built without the material system"; return {};
#endif
    }

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
                if (ImGui::Selectable((std::string("  ")+objects_[i].name).c_str(), sel_==i)) { sel_=i; selEntity_=scene::kInvalidEntity; }
#if AVER_MODULE_SCENE
        {
            scene::World& w = scene::World::instance();
            const u32 n = w.count();
            int listed = 0;
            for (u32 i = 0; i < n; ++i) {
                const scene::Entity ent = w.at(i);
                if (!w.valid(ent) || w.destroyPending(ent)) continue;
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
        if (ImGui::Selectable("  Directional Light (Sun)", sel_==-2)) { sel_=-2; selEntity_=scene::kInvalidEntity; }
        if (ImGui::Selectable("  Sky + Atmosphere", sel_==-3))        { sel_=-3; selEntity_=scene::kInvalidEntity; }
        if (ImGui::Selectable("  Post Process", sel_==-4))            { sel_=-4; selEntity_=scene::kInvalidEntity; }
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

        flushEditorPrefs();
    }

    // Draws the Editor Preferences window: how this machine's editor behaves.
    void buildEditorPrefs() {
        resolvePreferredIdeFromPrefs();
        if (!showEditorPrefs_) return;
        const ImGuiViewport* mv = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(560.0f*dpi_, 460.0f*dpi_), ImGuiCond_FirstUseEver);
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
        }
        if (ImGui::CollapsingHeader("Viewport", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Show grid", &showGrid_);
            ImGui::Checkbox("Wireframe", &wireframe_);
            ImGui::SliderFloat("Fly speed (cm/s)", &flySpeed_, 20.0f, 20000.0f, "%.0f",
                               ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Look sensitivity", &lookSpeed_, 0.001f, 0.02f, "%.4f");
        }
        ImGui::Separator();
        ImGui::TextDisabled("Preferences apply immediately and are saved for next time.");
        ImGui::TextDisabled("%s", editor::editorPrefsPath().c_str());
        ImGui::End();

        saveEditorPreferences();
    }

    // Draws the Project Settings window: a category sidebar beside the selected settings page.
    void buildProjectSettings() {
        if (focusVoxi_ > 0) { showProjectSettings_ = true; --focusVoxi_; } // --project-settings
        if (!showProjectSettings_) return;

        const ImGuiViewport* mv = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(880.0f*dpi_, 560.0f*dpi_), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(mv->GetCenter().x, mv->GetCenter().y), ImGuiCond_FirstUseEver, ImVec2(0.5f,0.5f));
        if (!ImGui::Begin("Project Settings", &showProjectSettings_, ImGuiWindowFlags_NoDocking)) { ImGui::End(); return; }

        ImGui::BeginChild("##categories", ImVec2(220.0f*dpi_, 0), ImGuiChildFlags_Borders);
        ImGui::TextDisabled("Project");
        ImGui::Indent();
        if (ImGui::Selectable("Description", settingsPage_==0)) settingsPage_=0;
        ImGui::Unindent();
        ImGui::TextDisabled("Engine");
        ImGui::Indent();
        if (ImGui::Selectable("Rendering", settingsPage_==1)) settingsPage_=1;
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
            buildRenderingSettings();
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
    // Draws the Voxi rendering settings page. Each feature reports its real status and is disabled
    // when the renderer or the GPU cannot do it.
    void buildRenderingSettings() {
        using namespace aver::voxi;
        Renderer& vx = Renderer::get();
        ImGui::TextUnformatted("Rendering");
        ImGui::SameLine(); ImGui::TextDisabled("(Voxi render module)");
        ImGui::Separator();

        Settings s = vx.settings();
        bool changed = false;
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);

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

        auto qualityRow = [&](Feature f, Quality& slot) {
            ImGui::Separator();
            const Status st = vx.status(f);
            ImGui::TextUnformatted(Renderer::featureName(f));
            const ImVec4 col = st==Status::Ready ? ImVec4(0.45f,0.85f,0.45f,1)
                             : st==Status::NotImplemented ? ImVec4(0.95f,0.72f,0.25f,1)
                                                          : ImVec4(0.75f,0.35f,0.35f,1);
            ImGui::SameLine(); ImGui::TextColored(col, "[%s]", vx.statusText(f));
            ImGui::BeginDisabled(st != Status::Ready);
            int q = static_cast<int>(slot);
            const char* qs[] = {"Off","Low","Medium","High","Epic"};
            ImGui::PushID(static_cast<int>(f));
            if (ImGui::Combo("Quality", &q, qs, 5)) { slot = static_cast<Quality>(q); changed = true; }
            ImGui::PopID();
            ImGui::EndDisabled();
        };
        qualityRow(Feature::GlobalIllumination, s.globalIllumination);

        ImGui::BeginDisabled(vx.status(Feature::GlobalIllumination) != Status::Ready);
        int res = static_cast<int>(s.voxelResolution);
        const char* resLabels[] = {"64", "128", "256"};
        const int resValues[] = {64, 128, 256};
        int resIdx = res>=256 ? 2 : (res>=128 ? 1 : 0);
        if (ImGui::Combo("Voxel grid", &resIdx, resLabels, 3)) { s.voxelResolution = (u32)resValues[resIdx]; changed = true; }
        if (ImGui::SliderFloat("GI intensity", &s.giIntensity, 0.0f, 4.0f)) changed = true;
        if (ImGui::SliderFloat("GI distance", &s.giMaxDistance, 10.0f, 20000.0f, "%.0f")) changed = true;
        ImGui::DragFloat3("Volume centre", &giCenter_.x, 0.5f);
        ImGui::DragFloat("Volume extent", &giExtent_, 0.5f, 1.0f, 100000.0f);
        ImGui::Checkbox("Debug: show voxel radiance", &giDebugView_);
        ImGui::EndDisabled();

        qualityRow(Feature::RayTracing,  s.rayTracing);
        qualityRow(Feature::PathTracing, s.pathTracing);

        ImGui::Separator();
        {
            const Status st = vx.status(Feature::MeshShaders);
            ImGui::TextUnformatted(Renderer::featureName(Feature::MeshShaders));
            ImGui::SameLine();
            const ImVec4 col = st==Status::Ready ? ImVec4(0.45f,0.85f,0.45f,1)
                             : st==Status::NotImplemented ? ImVec4(0.95f,0.72f,0.25f,1)
                                                          : ImVec4(0.75f,0.35f,0.35f,1);
            ImGui::TextColored(col, "[%s]", vx.statusText(Feature::MeshShaders));
            ImGui::BeginDisabled(st != Status::Ready);
            if (ImGui::Checkbox("Use mesh shaders", &s.meshShaders)) changed = true;
            ImGui::EndDisabled();
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
            for (f32 v : opts){ char b[24]; std::snprintf(b,sizeof b,"%g\xC2\xB0", v); if (ImGui::Selectable(b, rotSnap_==v)){ rotSnap_=v; snapRot_=true; } }
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
        ImGui::Text("%s  |  RMB fly (WASD/QE)  wheel speed  MMB pan  F focus  |  1-4 tools", kToolNames[(int)tool_]);
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
    scene::Entity selEntity_ = scene::kInvalidEntity;
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
    std::unordered_map<EditId, scene::Entity> editToEntity_;
    std::unordered_map<u32, EditId> entityToEdit_;
    EditId nextEditId_ = 1;
    EditXform editBefore_{};
    bool editBeforeValid_ = false;

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
    f32 prevMouseX_=0, prevMouseY_=0;
    // snapping (off by default; toggled from the toolbar carets)
    bool snapMove_=false, snapRot_=false, snapScale_=false;
    f32 moveSnap_=1.0f, rotSnap_=15.0f, scaleSnap_=0.25f;
    // frame state
    f32 dpi_=1.0f;
#if AVER_WITH_IMGUI
    ImFont* fontMedium_=nullptr; // Roboto Medium, for the menu bar; null if only the fallback loaded
#endif
    // 3D viewport rect, in backbuffer pixels. Latched by buildUI, consumed the next frame.
    f32 vpX_=0, vpY_=0, vpW_=1600, vpH_=900;
    bool dockBuilt_=false;   // one-shot DockBuilder layout (nothing is persisted to an ini)
    bool showProjectSettings_=false; // Edit > Project Settings window
    bool showEditorPrefs_=false;     // Edit > Editor Preferences window
    int  settingsPage_=1;            // 0 = Description, 1 = Rendering
    int  focusVoxi_=0;               // --project-settings: frames left to force the window open
    int  msaaOverride_=0;            // --msaa N: apply a sample count at startup
    int  giOverride_=0;              // --gi: GI quality to apply at startup
    bool giForceOff_=false;          // --no-gi: force it off, whatever the default is
    int  rtOverride_=0;              // --rt: ray tracing quality at startup
    bool msOverride_=false;          // --ms: force the mesh shader geometry path
    u32  probeX_=0, probeY_=0;       // --probe X Y: absolute capture pixel (0 = viewport centre)
    f32  probeU_=-1.0f, probeV_=-1.0f;   // --probe-rel U V: a FRACTION of the viewport rect
    bool useWarp_=false;             // --warp: run on the D3D12 software rasteriser
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
    std::unordered_map<std::string, std::pair<std::filesystem::file_time_type, int>> fileIconCache_;
    std::unordered_map<std::string, DirListing> dirCache_;
    int frameNo_ = 0;                                      // bumped once per UI frame; the cache freshness clock
    rhi::TextureHandle compileIconTexture_=0;   // the Compile C# status sprite sheet (3 tiles)
    u64 compileIconUiId_=0;
    u64 logoUiId_=0;
    f32 logoAspect_=1.0f;
    editor::ToolsMenu tools_;
    bool worldSpace_=true;   // gizmo coordinate space toggle (display only for now)
    bool giDebugView_=false; Vec3 giCenter_{0,0,300}; f32 giExtent_=1200.0f;   // cm
#if AVER_MODULE_VOXI
    voxi::VoxiRenderer voxiRenderer_;
    bool voxiAttached_=false;
#endif
#if AVER_MODULE_SCRIPTING
    scripting::ScriptHost scripts_;
#endif
    // The retained game UI's renderer. Heap-owned because create() may decline.
    aver::render::ui::UiRenderer* gameUi_ = nullptr;
    bool showUiDemo_ = false;
    std::string matSaveStatus_;   // what the last 'Save to C#' did
    unsigned centralDock_ = 0;    // the dock node an opened asset editor lands in
    bool levelFocused_ = true;    // the Level tab holds the keyboard
    bool levelHovered_ = true;    // the cursor is over the Level tab and it is topmost there
    bool inputProbe_ = false;
    bool levelVisible_ = true;    // the Level tab is the selected tab
    std::string openAsset_;

    editor::ProjectUpgrade pendingUpgrade_;
    bool        upgradeAsked_ = false;
    std::string upgradeStatus_;
    f32  uiDemoHealth_ = 0.72f, uiDemoStamina_ = 0.44f, uiDemoScroll_ = 0.0f, uiDemoClock_ = 0.0f;

    rhi::MeshHandle cubeMesh_=0; u32 cubeTris_=0; int spawnCount_=0;
    // The built-in look for a named surface with no material asset behind it.
    struct SurfaceLook { f32 col[3]; f32 metallic; f32 roughness; };
    std::unordered_map<i32, SurfaceLook> surfaceLooks_;
#if AVER_MODULE_PBR
    rhi::IResourceFactory* textureFactory_ = nullptr;   // cached: the resolver is a static callback
    // fnv1a64(content-relative path) -> absolute path, for `{guid:...}` texture references.
    std::unordered_map<u64, std::string> contentIndex_;
    // Surface name -> its .ocmat's material. 0 is a cached negative, not a miss to retry.
    std::unordered_map<std::string, pbr::MaterialHandle> materialAssets_;
    // The same answer keyed by the token the scene interns, which is what a CMeshRenderer carries.
    std::unordered_map<i32, pbr::MaterialHandle> surfaceMaterials_;
#endif
#if AVER_MODULE_SCENE
    // Loads an .ocworld into the world as ordinary scene entities: transform, mesh and name.
    void loadLevel(const std::string& path) {
        unloadLevel();
        fmt::OcWorldData w;
        std::string why;
        if (!fmt::loadOcworld(path, w, &why)) { AVER_WARN("[Level] {}", why); return; }

        scene::World& world = scene::World::instance();
        for (const fmt::OcWorldPlacement& p : w.placements) {
            Transform xf;
            xf.position = Vec3{static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)};
            xf.rotation = quatFromEulerDeg(Vec3{static_cast<f32>(p.roll), static_cast<f32>(p.pitch),
                                                static_cast<f32>(p.yaw)});
            xf.scale = Vec3{static_cast<f32>(p.sx), static_cast<f32>(p.sy), static_cast<f32>(p.sz)};

            const scene::Entity e = world.create(p.asset, scene::kInvalidEntity, xf);
            if (e == scene::kInvalidEntity) continue;
            auto* mr = static_cast<scene::CMeshRenderer*>(world.addComponent(e, scene::kComponentMeshRenderer));
            if (mr) {
                mr->mesh = p.objectId;
                mr->material = p.material.empty() ? 0 : aver_scene_material(0, p.material.c_str());
                mr->flags |= scene::kMeshRendererVisible;
#if AVER_MODULE_PBR
                if (mr->material) {
                    const pbr::MaterialHandle h = materialForSurface(p.material);
                    if (h) surfaceMaterials_[mr->material] = h;
                }
#endif
            }
            levelEntities_.push_back(e);
            entityLabels_[static_cast<u32>(e)] = makeEntityLabel(p.material, p.asset);

#if AVER_MODULE_PHYSICS
            if (p.collide && aver_phys_ready()) {
                const int32_t body = aver_phys_add_static_box(
                    static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z),
                    static_cast<f32>(p.sx), static_cast<f32>(p.sy), static_cast<f32>(p.sz));
                levelBodies_.push_back(body);
                entityBodies_[static_cast<u32>(e)] = body;
            }
#endif
        }

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
        Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        for (const fmt::OcWorldPlacement& p : w.placements) {
            const Vec3 c{static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)};
            const Vec3 e{static_cast<f32>(std::fabs(p.sx)), static_cast<f32>(std::fabs(p.sy)),
                         static_cast<f32>(std::fabs(p.sz))};
            lo.x = std::fmin(lo.x, c.x - e.x); hi.x = std::fmax(hi.x, c.x + e.x);
            lo.y = std::fmin(lo.y, c.y - e.y); hi.y = std::fmax(hi.y, c.y + e.y);
            lo.z = std::fmin(lo.z, c.z - e.z); hi.z = std::fmax(hi.z, c.z + e.z);
        }
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

#if AVER_MODULE_VOXI
        giCenter_ = centre;
        giExtent_ = radius;
#endif
    }

    // Loads the project's start map, resolved against its content directory. Missing is not an error.
    void loadStartMap() {
        if (!project_.valid() || project_.startMap.empty()) return;
        const std::string path = project_.contentDir() + "\\" + project_.startMap;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            AVER_INFO("[Level] start map '{}' does not exist yet - the world starts empty", project_.startMap);
            levelName_ = std::filesystem::path(project_.startMap).stem().string();
            levelPath_ = path;
            return;
        }
        loadLevel(path);
    }

    // Destroys the loaded level's entities and everything keyed to them: labels, bodies, undo.
    void unloadLevel() {
        scene::World& world = scene::World::instance();
        for (const scene::Entity e : levelEntities_) if (world.valid(e)) world.destroy(e);
        levelEntities_.clear();
        entityLabels_.clear();
        labelCounts_.clear();
        entityBodies_.clear();
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
    }

    // Writes the level's own entities back out to an .ocworld. Spawned actors are not written.
    bool saveLevel(const std::string& path) {
        scene::World& world = scene::World::instance();
        fmt::OcWorldData w;
        w.name = levelName_.empty() ? std::string("untitled") : levelName_;
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
            p.collide = true;
            (void)mr;
            w.placements.push_back(std::move(p));
        }

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
#if AVER_MODULE_PHYSICS
    std::vector<int32_t> levelBodies_;
#endif
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
                if (a.args.empty()) { w = "tool needs a tool index 0-3"; return false; }
                const int t = static_cast<int>(a.args[0]);
                if (t < 0 || t > 3) { w = "tool index out of range 0-3"; return false; }
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
#if AVER_MODULE_PHYSICS
    // The world's floor: a 100m square, 10cm thick, centred so its top face sits on z=0.
    static constexpr f32 kGroundHalfExtentCm = 5000.0f;
    static constexpr f32 kGroundHalfThickCm  = 5.0f;
    int32_t groundBody_ = 0;
#endif
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
    int lastSceneDrawn_=-1;           // last scene-entity draw count, so the log line fires only on change
#endif
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
    u64 frames=0; bool headless=false, focusVoxi=false, focusScript=false, focusTools=false, focusCompileMenu=false, focusCompile=false, startScreen=false; int drawerOpen=0; std::string drawerSub; std::string beam, shot, project, scriptsDir, spawnTest; bool playTest=false; Tool tool=Tool::Select; int msaa=0; int gi=0; int rt=0; bool noGi=false; bool giDbg=false, ms=false; u32 probeX=0, probeY=0; f32 probeU=-1.0f, probeV=-1.0f; int reloadAt=0; bool warp=false, debugLayer=false; const char* forceCaps=nullptr; f32 bloom=0.0f, exposure=1.0f; bool autoExposure=false; int clouds=0; f32 cloudCover=-1.0f; bool skyPhysical=false, skyAuthored=false; f32 skyElevation=-999.0f; bool vsyncOff=false; bool uiDemo=false; bool inputProbe=false; bool autoCompile=false; bool showPrefs=false; bool saveProject=false; std::string importSrc, importDst; int focusLevelAt=0; int hudTest=-1; std::string openAsset;
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
        // --open-asset <path> opens a file through the same host a double-click goes through.
        else if (!std::strcmp(argv[i],"--open-asset") && i+1<argc) openAsset=argv[++i];
        // --actor-live brings an actor tab up with LIVE already on.
        else if (!std::strcmp(argv[i],"--actor-live")) editor::setActorEditorLiveByDefault(true);
        else if (!std::strcmp(argv[i],"--headless")) headless=true;
        else if (!std::strcmp(argv[i],"--input-probe")) inputProbe=true;
        else if (!std::strcmp(argv[i],"--auto-compile")) autoCompile=true;
        else if (!std::strcmp(argv[i],"--focus-level-at") && i+1<argc) focusLevelAt=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--project-settings")) focusVoxi=true;
        else if (!std::strcmp(argv[i],"--editor-prefs")) showPrefs=true;
        else if (!std::strcmp(argv[i],"--hud-preview") && i+1<argc) hudTest=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--save-project")) saveProject=true;
        else if (!std::strcmp(argv[i],"--import") && i+2<argc) { importSrc=argv[++i]; importDst=argv[++i]; }
        else if (!std::strcmp(argv[i],"--new-script")) focusScript=true;
        else if (!std::strcmp(argv[i],"--tools-menu")) focusTools=true;
        else if (!std::strcmp(argv[i],"--compile-menu")) focusCompileMenu=true;
        // --mcp [port] opens the editor control channel. Opt-in: it is a listening socket.
        else if (!std::strcmp(argv[i],"--mcp")) {
            mcpPort = 45123;
            if (i+1 < argc && argv[i+1][0] != '-') mcpPort = (u16)std::atoi(argv[++i]);
        }
        else if (!std::strcmp(argv[i],"--compile-scripts")) focusCompile=true;
        // --reload-scripts [N] fires Tools > Reload Scripts once, N frames in (default 20).
        else if (!std::strcmp(argv[i],"--reload-scripts")) {
            reloadAt = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 20;
        }
        else if (!std::strcmp(argv[i],"--start-screen")) startScreen=true;
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
        else if (!std::strcmp(argv[i],"--rt")) rt=3;
        else if (!std::strcmp(argv[i],"--ms")) ms=true;
        else if (!std::strcmp(argv[i],"--probe") && i+2<argc) { probeX=(u32)std::atoi(argv[++i]); probeY=(u32)std::atoi(argv[++i]); }
        // --force-caps clamps what the device reports; it can never raise a capability.
        else if (!std::strcmp(argv[i],"--force-caps") && i+1<argc) forceCaps=argv[++i];
        else if (!std::strcmp(argv[i],"--warp")) warp=true;
        else if (!std::strcmp(argv[i],"--debug-layer")) debugLayer=true;
        else if (!std::strcmp(argv[i],"--scripts") && i+1<argc) scriptsDir=argv[++i];
        else if (!std::strcmp(argv[i],"--spawn-test") && i+1<argc) spawnTest=argv[++i];
        else if (!std::strcmp(argv[i],"--play-test")) playTest=true;
        else if (!std::strcmp(argv[i],"--frames") && i+1<argc) frames=std::strtoull(argv[++i],nullptr,10);
        else if (!std::strcmp(argv[i],"--screenshot") && i+1<argc) shot=argv[++i];
        else if (!std::strcmp(argv[i],"--bloom") && i+1<argc) bloom=static_cast<f32>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i],"--exposure") && i+1<argc) exposure=static_cast<f32>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i],"--auto-exposure")) autoExposure=true;
        else if (!std::strcmp(argv[i],"--no-vsync")) vsyncOff=true;
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
        else if (!std::strcmp(argv[i],"--tool") && i+1<argc) {
            const char* t=argv[++i];
            tool = !std::strcmp(t,"move")?Tool::Move : !std::strcmp(t,"rotate")?Tool::Rotate :
                   !std::strcmp(t,"scale")?Tool::Scale : Tool::Select;
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
    if (clouds) app->setClouds(cloudCover);
    if (skyPhysical) app->setSkyPhysical(skyElevation);
    if (skyAuthored) app->setSkyAuthored();
    app->setVSyncOff(vsyncOff);
    app->setUiDemo(uiDemo);
    app->setOpenAsset(openAsset);
    app->setInputProbe(inputProbe);
    app->setAutoCompile(autoCompile);
    app->setFocusLevelAt(focusLevelAt);
    app->setShowEditorPrefs(showPrefs);
    app->setHudTest(hudTest);
    app->setSaveProject(saveProject);
    app->setImportOnce(importSrc, importDst);
    app->setUseWarp(warp);
    app->setDebugLayer(debugLayer);
    app->setProjectPath(project);
    // The start screen: interactive launches with no project, or --start-screen. Never in a capture run.
    app->armBrowser(startScreen || (!headless && frames == 0 && project.empty()));
    app->setFocusVoxi(focusVoxi);
    app->setDrawerOpen(drawerOpen, drawerSub);
    app->setFocusScript(focusScript);
    app->setFocusTools(focusTools);
    app->setFocusCompileMenu(focusCompileMenu);
#if AVER_MODULE_MCP
    app->setMcpPort(mcpPort);
#else
    if (mcpPort) AVER_WARN("[Mcp] --mcp was given but this build has no control channel "
                           "(-DAVER_MODULE_MCP=ON to include it); the editor runs regardless");
#endif
    app->setFocusCompile(focusCompile);
    app->setFocusReload(reloadAt);
    app->setMsaaOverride(msaa);
    app->setGiOverride(gi, giDbg);
    app->setRtOverride(rt);
    app->setMsOverride(ms);
    app->setProbe(probeX, probeY);
    if (probeU >= 0.0f) app->setProbeRel(probeU, probeV);
    app->setScriptsDir(scriptsDir);
    app->setSpawnTest(spawnTest);
    if (playTest) app->setPlayTest();
    return app;
}

} // namespace aver
