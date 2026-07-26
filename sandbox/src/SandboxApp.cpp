#include "aver/runtime/EntryPoint.hpp"
#include "aver/platform/Window.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/platform/Image.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/core/Hash.hpp"          // fnv1a64: the mesh-path -> ObjectId hash the C# ClassBuilder uses
#include "aver/core/Version.hpp"
#include "aver/formats/OcBeam.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/formats/OcWorld.hpp"      // .ocworld: the level format the editor loads and saves

#include "ProjectBrowser.hpp"
#include "ToolsMenu.hpp"
#include "EngineScaffold.hpp"   // engineRoot(): where the Content Browser's "Engine" root is mounted from
#include "IdeIntegration.hpp"   // detectedIdes()/openInIde: double-clicking a source file opens it
#include "ShellIntegration.hpp" // reveal / shell-open / recycle, for the browser's context menu

#if AVER_MODULE_VOXI
#include "aver/voxi/Voxi.hpp"          // optional render-feature module (AA / GI / RT / PT settings)
#include "aver/voxi/VoxiRenderer.hpp"  // ...and its GPU side, registered as an rhi::IRenderFeature
#endif

#if AVER_MODULE_PBR
#include "aver/pbr/Material.hpp"       // the authored surface, from the Core-only material DLL
#include "aver/pbr/MaterialGpu.hpp"    // MaterialConstants: the block setDrawBinding carries
#include "aver/formats/OcMat.hpp"      // .ocmat: a surface is an ASSET now, not a hardcoded palette row
#include "aver/assets/TextureUpload.hpp" // and the decode-to-GPU step behind the texture resolver
#endif

#if AVER_MODULE_SCRIPTING
#include "aver/scripting/ScriptHost.hpp" // in-process CLR host; declines when .NET is absent
#endif

#if AVER_MODULE_FRAMEWORK
#if AVER_MODULE_PHYSICS
#include "aver/physics/physics_abi.h"       // the simulation the frame loop steps between tick groups
#endif
#include "aver/framework/framework_abi.h"   // aver_fw_class_find / aver_fw_spawn (the --spawn-test path)
#include "aver/framework/framework_hooks.h" // aver_fw_tick — drive the managed tick groups per frame
#endif

#if AVER_MODULE_SCENE
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>                  // cursor capture while a game has the mouse
#endif
#include "aver/scene/scene_abi.h"     // aver_scene_material: interning the names the surface palette keys on
#include "aver/scene/World.hpp"       // the one world a spawned actor lives in — walked by the render pass
#include "aver/scene/Components.hpp"  // CMeshRenderer / CWorld layout, read directly through the pools
#endif

#if AVER_WITH_IMGUI
#include "imgui.h"
#include "imgui_internal.h" // DockBuilder* (docking layout is built in code: IniFilename is null)
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
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aver {

// Engine axis convention: +X forward, +Y right, +Z up (see Math.hpp). Gizmo colours
// follow the usual X=red, Y=green, Z=blue mapping; highlight = amber.
static const Vec3 kAxisDir[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
static const Vec3 kAxisCol[3] = {{0.92f, 0.24f, 0.24f}, {0.36f, 0.82f, 0.30f}, {0.30f, 0.55f, 1.0f}};
static const Vec3 kAxisHi = {1.0f, 0.80f, 0.15f};

static void appendBox(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx, f32 cx, f32 cy, f32 cz, f32 h) {
    const f32 p[8][3] = {{-h,-h,-h},{h,-h,-h},{h,h,-h},{-h,h,-h},{-h,-h,h},{h,-h,h},{h,h,h},{-h,h,h}};
    struct Face { f32 n[3]; int c[4]; };
    const Face faces[6] = {{{1,0,0},{1,2,6,5}},{{-1,0,0},{0,4,7,3}},{{0,1,0},{3,7,6,2}},
                           {{0,-1,0},{0,1,5,4}},{{0,0,1},{4,5,6,7}},{{0,0,-1},{0,3,2,1}}};
    // A box is six planar quads, so its UVs are exact rather than projected: each face's four
    // corners are emitted in ring order, which is the unit square's corners in ring order.
    const f32 quadUV[4][2] = {{0,0},{1,0},{1,1},{0,1}};
    for (const Face& f : faces) {
        const u32 b = static_cast<u32>(v.size());
        for (int k = 0; k < 4; ++k) { const f32* c = p[f.c[k]]; v.push_back({cx+c[0],cy+c[1],cz+c[2],f.n[0],f.n[1],f.n[2],quadUV[k][0],quadUV[k][1]}); }
        idx.push_back(b); idx.push_back(b+1); idx.push_back(b+2); idx.push_back(b); idx.push_back(b+2); idx.push_back(b+3);
    }
}
static void appendGround(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx, f32 s) {
    const u32 b = static_cast<u32>(v.size());
    // Ground UV is world XY scaled by the quad's extent, so a tiling material keeps a constant
    // texel density however large the ground is made.
    v.push_back({-s,-s,0,0,0,1,-0.5f,-0.5f}); v.push_back({s,-s,0,0,0,1,0.5f,-0.5f});
    v.push_back({s,s,0,0,0,1,0.5f,0.5f}); v.push_back({-s,s,0,0,0,1,-0.5f,0.5f});
    idx.push_back(b); idx.push_back(b+1); idx.push_back(b+2); idx.push_back(b); idx.push_back(b+2); idx.push_back(b+3);
}
// A UV sphere, +Z as the pole to match the engine's up axis. On a unit sphere the outward normal IS
// the position, so nx/ny/nz reuse the vertex directly. Emitted as a grid of `rings` latitude bands by
// `sectors` longitude columns; the seam column is duplicated so its U wraps 1.0 rather than back to 0.
static void appendSphere(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx, f32 r, u32 rings, u32 sectors) {
    const u32 base = static_cast<u32>(v.size());
    for (u32 ring = 0; ring <= rings; ++ring) {
        const f32 phi = kPi * (static_cast<f32>(ring) / static_cast<f32>(rings)); // 0 at +Z pole -> pi at -Z
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
static Quat quatFromEulerDeg(const Vec3& e) {
    return (Quat::fromAxisAngle({0,0,1}, radians(e.z)) * Quat::fromAxisAngle({0,1,0}, radians(e.y)) *
            Quat::fromAxisAngle({1,0,0}, radians(e.x))).normalized();
}
// The exact inverse of quatFromEulerDeg above, returning the same (roll, pitch, yaw) packing.
//
// It has to be the inverse of THAT composition specifically -- Rz(yaw) * Ry(pitch) * Rx(roll) -- and
// not a generic euler extraction, or a level would not round-trip: save then load would rotate every
// placement slightly, and the drift would compound with each save.
static Vec3 eulerDegFromQuat(const Quat& q) {
    const f32 sinP = 2.0f * (q.w * q.y - q.z * q.x);
    const f32 pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, sinP)));
    f32 roll, yaw;
    if (std::fabs(sinP) > 0.99999f) {
        // Straight up or down: roll and yaw describe the same rotation, so pin roll and put all of it
        // in yaw rather than letting the atan2s return an arbitrary split of it.
        roll = 0.0f;
        yaw  = std::atan2(-2.0f * (q.x * q.y - q.w * q.z), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
    } else {
        roll = std::atan2(2.0f * (q.w * q.x + q.y * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        yaw  = std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
    }
    const f32 r2d = 180.0f / 3.14159265358979323846f;
    return Vec3{roll * r2d, pitch * r2d, yaw * r2d};
}
static f32 snapf(f32 v, f32 step) { return step > 0.0f ? std::round(v / step) * step : v; }

// Row-vector transforms (v * M) for picking.
static Vec3 xformPoint(const Mat4& m, const Vec3& p) {
    return { p.x*m.m[0][0]+p.y*m.m[1][0]+p.z*m.m[2][0]+m.m[3][0],
             p.x*m.m[0][1]+p.y*m.m[1][1]+p.z*m.m[2][1]+m.m[3][1],
             p.x*m.m[0][2]+p.y*m.m[1][2]+p.z*m.m[2][2]+m.m[3][2] };
}
static Vec3 xformVec(const Mat4& m, const Vec3& v) {
    return { v.x*m.m[0][0]+v.y*m.m[1][0]+v.z*m.m[2][0],
             v.x*m.m[0][1]+v.y*m.m[1][1]+v.z*m.m[2][1],
             v.x*m.m[0][2]+v.y*m.m[1][2]+v.z*m.m[2][2] };
}
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
static void buildGrid(std::vector<rhi::LineVertex>& v, f32 ext, f32 step) {
    const f32 g = 0.26f;
    for (f32 x = -ext; x <= ext + 0.001f; x += step) {
        v.push_back({x, -ext, 0.02f, g, g, g}); v.push_back({x, ext, 0.02f, g, g, g});
    }
    for (f32 y = -ext; y <= ext + 0.001f; y += step) {
        v.push_back({-ext, y, 0.02f, g, g, g}); v.push_back({ext, y, 0.02f, g, g, g});
    }
    v.push_back({0,0,0.03f, 0.80f,0.25f,0.25f}); v.push_back({ext,0,0.03f, 0.80f,0.25f,0.25f}); // +X (forward)
    v.push_back({0,0,0.03f, 0.28f,0.72f,0.30f}); v.push_back({0,ext,0.03f, 0.28f,0.72f,0.30f}); // +Y (right)
}

// ---- per-mode gizmo geometry (unit-size, local space; scaled by the world matrix) ----
static void gzLine(std::vector<rhi::LineVertex>& v, const Vec3& a, const Vec3& b, const Vec3& c) {
    v.push_back({a.x,a.y,a.z, c.x,c.y,c.z}); v.push_back({b.x,b.y,b.z, c.x,c.y,c.z});
}
static std::vector<rhi::LineVertex> buildMoveAxis(int a, const Vec3& c) {
    std::vector<rhi::LineVertex> v;
    const Vec3 A = kAxisDir[a], P = kAxisDir[(a+1)%3], Q = kAxisDir[(a+2)%3];
    gzLine(v, {0,0,0}, A, c);                                  // shaft
    const Vec3 tip = A, base = A * 0.80f;                      // conical arrowhead
    for (int k = 0; k < 4; ++k) { f32 t = k * (kPi * 0.5f); Vec3 r = P*(std::cos(t)*0.07f) + Q*(std::sin(t)*0.07f); gzLine(v, base+r, tip, c); }
    return v;
}
static std::vector<rhi::LineVertex> buildRotRing(int a, const Vec3& c) {
    std::vector<rhi::LineVertex> v;
    const Vec3 P = kAxisDir[(a+1)%3], Q = kAxisDir[(a+2)%3];
    const int N = 64; Vec3 prev{};
    for (int k = 0; k <= N; ++k) { f32 t = k * (kTwoPi / N); Vec3 p = P*std::cos(t) + Q*std::sin(t); if (k > 0) gzLine(v, prev, p, c); prev = p; }
    return v;
}
static std::vector<rhi::LineVertex> buildScaleAxis(int a, const Vec3& c) {
    std::vector<rhi::LineVertex> v;
    const Vec3 A = kAxisDir[a], P = kAxisDir[(a+1)%3], Q = kAxisDir[(a+2)%3];
    gzLine(v, {0,0,0}, A * 0.86f, c);                         // shaft
    const Vec3 ctr = A * 0.93f; const f32 h = 0.07f;          // small box at the tip
    Vec3 cor[8]; int i = 0;
    for (int sx = -1; sx <= 1; sx += 2) for (int sy = -1; sy <= 1; sy += 2) for (int sz = -1; sz <= 1; sz += 2)
        cor[i++] = ctr + A*(h*sx) + P*(h*sy) + Q*(h*sz);
    const int e[12][2] = {{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
    for (auto& pr : e) gzLine(v, cor[pr[0]], cor[pr[1]], c);
    return v;
}

enum class Tool { Select, Move, Rotate, Scale };
static const char* kToolNames[4] = {"Select", "Move", "Rotate", "Scale"};

// Which bottom drawer is up. Only one at a time: they share the same strip of screen, and a drawer
// that can be half-covered by its sibling is a layout, not a drawer.
enum class Drawer { None, Content, Log };

#if AVER_WITH_IMGUI
// Fixed editor chrome (toolbar / status bar / dock host): no decoration, never steals focus.
static constexpr ImGuiWindowFlags kChromeFlags =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

// The drawer differs from the chrome above in the one way that matters: it MAY come to the front,
// because it overlays the viewport and has to sit above the dock host.
//
// It must NOT scroll. Its bodies scroll inside their own children, so the outer window has nothing to
// scroll -- but its header is submitted before the body is size-gated, so mid-slide the content
// briefly exceeds the window and ImGui would flash a scrollbar on every close. Worse, the resize grip
// is positioned at content y=0, which is scroll-relative: one wheel tick would carry it above the top
// edge and out of reach.
static constexpr ImGuiWindowFlags kDrawerFlags =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings;
#endif

struct MeshObj {
    std::string name;
    rhi::MeshHandle mesh = 0;
    u32 tris = 0;
    Vec3 pos{0,0,0}, rotDeg{0,0,0}, scale{1,1,1};
    f32 color[4] = {0.8f,0.4f,0.25f,1};
    f32 metallic = 0.0f, roughness = 0.5f;
#if AVER_MODULE_PBR
    // The actor's material. 0 until onInit creates one, and 0 is the handle the material system
    // answers with the fallback for, so an actor added before the library exists still draws.
    //
    // metallic/roughness above are NOT dead while this is set: they are what reaches b1, and under
    // AVER_MODULE_PBR they are pinned to 1 so the material's factors carry the authored value
    // through the shader's factor * map product. Without the module they stay the authored values
    // and the frozen no-material path reads them directly. See onRender.
    pbr::MaterialHandle material = 0;
#endif
    bool visible = true;
    Vec3 aabbMin{-1,-1,-1}, aabbMax{1,1,1}; // local-space bounds (for picking)
};

#if AVER_WITH_IMGUI
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

// One entry in a Content Browser listing, with everything the views need already derived. The
// per-entry work (two wide->narrow path conversions, the extension fold, and the .cs classification's
// 8 KB read) used to run per entry PER FRAME; deriving it once per refresh is what keeps a folder of a
// few thousand files from costing more than the scene it sits under.
struct DirEntry {
    std::filesystem::path path;
    std::string full, name;
    bool isDir  = false;
    int  tile   = -1;      // file-type sprite tile, or -1 for "no icon for this type"
    bool module = false;   // a folder that is engine content or a C++ module -> the Module folder icon
};

// A throttled Content Browser directory listing, refreshed on a frame stamp. Folders sort first and
// are counted, so a view can split them without re-partitioning.
struct DirListing { int stamp = -1000; std::vector<DirEntry> entries; usize dirCount = 0; };

class SandboxApp final : public Application {
public:
    SandboxApp(u64 maxFrames, bool headless, std::string beamPath, std::string shot, Tool initialTool)
        : maxFrames_(maxFrames), headless_(headless), beamPath_(std::move(beamPath)), shot_(std::move(shot)), initialTool_(initialTool) {
        // Mirror every engine log line into the Output Log from the moment the app exists.
        setLogSink(&SandboxApp::logSink, this);
    }

    // Called (under the core log mutex, possibly off the UI thread) for every log line. Appends to a
    // bounded buffer the Output Log panel drains. Must not itself log — that would re-enter the held lock.
    static void logSink(void* ctx, LogLevel level, std::string_view msg) {
        auto* self = static_cast<SandboxApp*>(ctx);
        std::lock_guard<std::mutex> lock(self->logMutex_);
        self->logLines_.push_back({level, std::string(msg)});
        if (self->logLines_.size() > kMaxLogLines) self->logLines_.pop_front();
    }

    BootConfig config() const override {
        BootConfig c; c.windowTitle="Aver Engine \xE2\x80\x94 Editor"; c.windowWidth=1600; c.windowHeight=900;
        c.maxFrames=maxFrames_; c.headless=headless_; c.useWarp=useWarp_;
        c.enableDebugLayer=debugLayer_; return c;
    }
    void setUseWarp(bool w) { useWarp_ = w; }  // --warp
    void setDebugLayer(bool d) { debugLayer_ = d; }  // --debug-layer

#if AVER_WITH_IMGUI
    // Rebuild the style and the font atlas for `dpi`.
    //
    // The glyphs are rasterised at the physical size the display needs (16pt at 300% = 48px)
    // rather than asked for at 16px and scaled up afterwards. Note that the old
    // `io.FontGlobalScale = dpi` was NOT smearing a baked atlas: since 1.92 ImGui bakes glyphs on
    // demand, so that only multiplied the requested size and ProggyClean was re-rasterised, sharp,
    // at 39px. What made the editor look pixelated was purely that ProggyClean is a 13px pixel
    // font -- its outlines are square steps, so enlarging it gives clean but blocky letterforms.
    // Baking the size in is still the right shape for this: it is the supported spelling (the
    // global scale is the obsolete alias of style.FontScaleMain, and ImGui asserts if both are
    // used), and it keeps the size the atlas was built for explicit at the call site.
    //
    // ScaleAllSizes() is not idempotent, so the style is rebuilt from its unscaled base each time
    // rather than scaled again on top of an already-scaled style.
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

        // A missing font must never stop the editor coming up, so probe before asking ImGui to
        // load: AddFontFromFileTTF() raises a user assert on a file it cannot open.
        ImFont* body = fileExists(regular) ? io.Fonts->AddFontFromFileTTF(regular.c_str(), px) : nullptr;
        if (!body) {
            AVER_WARN("[Sandbox] '{}' missing or unreadable -- falling back to the built-in bitmap font", regular);
            io.Fonts->AddFontDefault();
            return;
        }
        io.FontDefault = body;
        if (fileExists(medium)) fontMedium_ = io.Fonts->AddFontFromFileTTF(medium.c_str(), px);
    }

    // Upload branding/logo.png (staged next to the exe) for the start screen.
    //
    // Called ONLY when the start screen is armed. Automation never shows it, and a decode plus a
    // megabyte of GPU upload is not a cost every --frames run should carry for a decoration.
    //
    // A missing or corrupt file is a warning, never a failure: the editor must still come up, and
    // the browser falls back to its drawn badge when the handle stays zero.
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
        td.initialRowPitch = img.rowPitch();   // ImageData is tightly packed RGBA
        logoTexture_ = res->createTexture(td);

        if (!logoTexture_) { AVER_WARN("[Sandbox] the start-screen mark could not be uploaded"); return; }
        logoUiId_ = e.device()->uiTextureId(logoTexture_);
        if (!logoUiId_) { AVER_WARN("[Sandbox] the start-screen mark is not reachable from the UI"); return; }
        logoAspect_ = h > 0 ? static_cast<f32>(w) / static_cast<f32>(h) : 1.0f;
        AVER_INFO("[Sandbox] start-screen mark decoded from {} ({}x{})", path, w, h);
    }

    // The Compile C# button's status icon: the compile-status sprite sheet (three tiles - built,
    // failed, stale) staged next to the exe. Loaded ALWAYS, unlike the logo, because the toolbar is
    // always up. A miss is a warning; the button falls back to a drawn dot.
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

    // Load one N-tile sprite sheet staged next to the exe. Shared by the Content Browser's file-type
    // and folder sheets: they differ only in the file and the tile count.
    //
    // The tile ASPECT is measured from the decoded image rather than assumed. Hard-coding it means a
    // sheet re-cut at another shape silently renders every icon stretched, which is the kind of fault
    // that survives review because each icon still looks like itself.
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

    void onInit(Engine& e) override {
        AVER_INFO("[Sandbox] backend={} adapter='{}'", rhi::backendName(e.device()->backend()), e.device()->adapterName());
        window_ = e.window();   // for the HWND the mouse capture needs

        // Always read the recent list, even when the start screen will not be shown: opening a
        // project records it, and recording into a list that was never loaded would truncate the
        // user's history to the one project the command line named.
        browser_.init();

#if AVER_MODULE_PHYSICS
        // Start the simulation BEFORE any project or level is opened.
        //
        // Ordering, not taste: loading a level builds a static body per colliding placement, and it
        // can only do that if the world exists. With this after the project load the level rendered
        // perfectly and had NO COLLISION -- shots passed through walls and the geometry was scenery.
        // Nothing errored, because "no physics yet" and "this placement does not collide" are the
        // same branch.
        //
        // Started here rather than under the UI branch: a headless --frames or --play-test run
        // simulates exactly like an interactive one, which is what makes the play test evidence.
        if (aver_phys_init()) {
            // The floor is a static box on the z=0 plane, in the ABI's centimetres. Its extent is
            // deliberately far larger than the visible grid: what matters for gameplay is that a
            // character cannot walk off the edge of the world, and a plane the eye reads as infinite
            // should behave that way.
            groundBody_ = aver_phys_add_static_box(0.0f, 0.0f, -kGroundHalfThickCm,
                                                   kGroundHalfExtentCm, kGroundHalfExtentCm,
                                                   kGroundHalfThickCm);
            AVER_INFO("[Sandbox] physics started, ground body={} (fixed step {:.4f}s)",
                      groundBody_, aver_phys_fixed_step());
        } else {
            AVER_WARN("[Sandbox] physics failed to start - gameplay will not collide");
        }
#endif
        // A project named on the command line is loaded straight away and the start screen is
        // skipped — see armBrowser() for why automation must never reach the browser.
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
            // (physics is started below, outside the UI branch -- a headless run simulates too)
            // The engine's C# classes, mounted as the Content Browser's second root. Absent from a
            // shipped build, where there is no source tree to point at.
            if (const std::string er = editor::engineRoot(); !er.empty()) {
                std::error_code ec;
                const std::filesystem::path cs = std::filesystem::path(er) / "scripting" / "csharp";
                if (std::filesystem::is_directory(cs, ec)) cbEngineRoot_ = cs.string();
            }
            AVER_INFO("[Sandbox] Content Browser engine root: {}",
                      cbEngineRoot_.empty() ? "(none - shipped build)" : cbEngineRoot_.c_str());
        }
#endif
        // --- default "blank .ocmap": ground floor + cube + sun + sky + atmosphere ---
        std::vector<rhi::MeshVertex> gv, gi_v; std::vector<u32> gi, ci;
        appendGround(gv, gi, 40.0f);
        rhi::MeshHandle ground = e.device()->createMesh(gv.data(), (u32)gv.size(), gi.data(), (u32)gi.size());
        appendBox(gi_v, ci, 0,0,0, 1.0f);
        rhi::MeshHandle cube = e.device()->createMesh(gi_v.data(), (u32)gi_v.size(), ci.data(), (u32)ci.size());

#if AVER_MODULE_SCENE
        // Register the built-in primitive meshes a SPAWNED actor's CMeshRenderer can name. A C# class
        // authored with `ClassBuilder.Mesh("Meshes/sphere.ocmesh")` writes fnv1a64(path) into the
        // component (Assets.ObjectIdOf); the scene-render pass in onRender resolves that same id back to
        // the handle through this table. The sphere is procedural rather than a loaded .ocmesh for now:
        // what step 7 needs is the id->handle RESOLUTION and the world walk, not an asset loader yet.
        {
            std::vector<rhi::MeshVertex> sv; std::vector<u32> si;
            appendSphere(sv, si, 1.0f, 24, 48);
            sceneMeshes_[fnv1a64(std::string_view("Meshes/sphere.ocmesh"))] =
                e.device()->createMesh(sv.data(), (u32)sv.size(), si.data(), (u32)si.size());
            // The unit cube, on the same terms as the sphere. A level blockout is boxes -- floors,
            // walls, platforms, crates -- so with only a sphere registered a gameplay script could
            // spawn actors but could not build anything to walk on or shoot at.
            sceneMeshes_[fnv1a64(std::string_view("Meshes/cube.ocmesh"))] = cube;
        }

        // The named surfaces gameplay can ask for. Chosen to READ rather than to be pretty: a floor
        // darker than its walls, warm crates against cool architecture, and one saturated accent kept
        // for the things the player is meant to shoot. Value separation is what makes a blockout
        // legible; hue on its own does not.
        {
            auto look = [this](const char* name, f32 r, f32 g, f32 b, f32 metal, f32 rough) {
                surfaceLooks_[aver_scene_material(0, name)] = SurfaceLook{{r, g, b}, metal, rough};
            };
            look("M_Floor",  0.22f, 0.23f, 0.26f, 0.02f, 0.85f);   // dark, so everything reads against it
            look("M_Wall",   0.48f, 0.50f, 0.55f, 0.03f, 0.72f);
            look("M_Trim",   0.30f, 0.33f, 0.38f, 0.35f, 0.45f);   // edges and platforms
            look("M_Crate",  0.62f, 0.44f, 0.22f, 0.02f, 0.78f);   // warm, against the cool room
            look("M_Target", 0.86f, 0.20f, 0.16f, 0.05f, 0.40f);   // the one saturated thing
            look("M_Metal",  0.55f, 0.57f, 0.60f, 0.85f, 0.28f);   // the gun
            look("M_Accent", 0.95f, 0.66f, 0.15f, 0.30f, 0.35f);
        }
#endif

        MeshObj floor; floor.name="Floor"; floor.mesh=ground; floor.tris=(u32)gi.size()/3;
        floor.color[0]=0.34f; floor.color[1]=0.35f; floor.color[2]=0.37f;
        floor.metallic=0.0f; floor.roughness=0.9f;
        floor.aabbMin=Vec3{-40,-40,-0.05f}; floor.aabbMax=Vec3{40,40,0.05f};
        objects_.push_back(floor);
        cubeMesh_ = cube; cubeTris_ = (u32)ci.size()/3; // reused by the toolbar's Add > Cube
        MeshObj c; c.name="Cube"; c.mesh=cube; c.tris=(u32)ci.size()/3; c.pos=Vec3{0,0,1};
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

        // Every actor gets a material. Done here, after the actor list is built, rather than inside
        // each construction: makeMaterialFor() reads the authored metallic/roughness off the actor
        // and then PINS the actor's own pair to 1, so running it twice on one actor would be
        // harmless but running it on half the list would not be obvious from the image.
        for (MeshObj& o : objects_) makeMaterialFor(o);

        std::vector<rhi::LineVertex> gl; buildGrid(gl, 40.0f, 2.0f);
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
            s.msaa = static_cast<voxi::Msaa>(e.device()->sampleCount()); // adopt the live value
            if (msaaOverride_) s.msaa = static_cast<voxi::Msaa>(msaaOverride_);
            if (giOverride_) s.globalIllumination = static_cast<voxi::Quality>(giOverride_);
            if (rtOverride_) s.rayTracing = static_cast<voxi::Quality>(rtOverride_);
            if (msOverride_) s.meshShaders = true;
            voxi::Renderer::get().setSettings(s);
            AVER_INFO("[Voxi] attached: MSAA {}x, RT tier {}, SM {}, mesh tier {}", c.maxMsaaSamples, c.rayTracingTier, c.shaderModel, c.meshShaderTier);

            // The render feature owns Voxi's GPU resources. Registration is non-owning, so the
            // member must outlive the device — it is torn down in onShutdown below.
            voxiRenderer_.setSettings(s);
            if (voxiRenderer_.init(*e.device())) {
                e.device()->addRenderFeature(&voxiRenderer_);
                voxiAttached_ = true;
#if AVER_MODULE_PBR
                // The last link in the material chain. MaterialSystem is built to resolve texture
                // references through a host-installed callback and, until this line, nothing
                // installed one -- so every slot in every material fell back to its 1x1 identity
                // texture and every surface the engine drew was a flat colour. The whole texture
                // path (decode, mip chain, sRGB view, upload) already existed on both sides of it.
                textureFactory_ = e.device()->resources();
                voxiRenderer_.materials().setTextureResolver(&SandboxApp::resolveMaterialTexture, this);
#endif
            }
        }
#endif
#if AVER_MODULE_SCRIPTING
        // Scripting is started LAST, after every rendering subsystem is up. It touches none of
        // them, so ordering is free -- and putting the one subsystem that may take a second to
        // start behind the ones the first frame actually needs keeps startup honest.
        //
        // A declined init is not an error path: no .NET runtime, no staged bridge, or a stale
        // bridge all end here with the editor running exactly as it does today. ScriptHost has
        // already logged the reason.
        {
            scripting::HostDesc hd;
            // The managed bridge is staged in bin/Scripting (step 11), not bin/: its managed Aver.Framework
            // /Aver.Scene copies share a file name with the native DLLs beside the exe and would collide.
            hd.bridgeDir = executableDir() + "\\Scripting";
            hd.scriptsDir = resolveScriptsDir();
            scripts_.init(hd);

            // How Tools > Reload Scripts reaches the host. A callback rather than a reference so
            // ToolsMenu never includes the scripting module — see ToolsMenu::ReloadFn. Installed
            // even when init declined: the host answers honestly either way, and a menu item that
            // reports "the scripting host is not running" is better than one that is greyed out
            // for a reason nobody can see.
            tools_.setReloader([this](const std::string& binDir, std::string* status) {
                return reloadScripts(binDir, status);
            });
        }
#endif
        tool_ = initialTool_;
        sel_ = 1; // the Cube
        // The default view frames the PLACEHOLDER scene. Skip it when a level was loaded above, which
        // has already framed the camera on itself -- this runs after applyProject, so without the
        // guard it silently puts the camera back inside a centimetre-scale level and the map looks
        // like it never loaded.
        bool framedByLevel = false;
#if AVER_MODULE_SCENE
        framedByLevel = !levelEntities_.empty();
#endif
        if (!framedByLevel) {
            camPos_ = Vec3{7.0f, 7.0f, 4.5f};
            const Vec3 d = (Vec3{0,0,1} - camPos_).getSafeNormal();
            yaw_ = std::atan2(d.y, d.x);
            pitch_ = std::asin(d.z);
        }
    }

    void onUpdate(Engine& e, const Timestep& t) override {
#if AVER_WITH_IMGUI
        if (e.device()->uiActive()) {
            // Dragging the window to a monitor with a different scale changes the size the glyphs
            // should have been rasterised at, so re-bake them. Safe here: onUpdate runs before
            // uiNewFrame(), i.e. outside the ImGui frame, and the atlas may not be touched inside
            // one. Win32Window latches the new scale from WM_DPICHANGED. Runs for the start screen
            // too — it is drawn with the same atlas.
            const f32 dpi = e.window() ? e.window()->dpiScale() : dpi_;
            if (std::fabs(dpi - dpi_) > 0.01f) {
                AVER_INFO("[Sandbox] DPI changed {:.2f} -> {:.2f}, re-rasterising the UI font", dpi_, dpi);
                applyDpi(dpi);
            }
        }
        // The camera must not fly and the viewport must not be dollied while the start screen is up.
        if (e.device()->uiActive() && !browserActive_) {
            const ImGuiIO& io = ImGui::GetIO();
            // The central dock node is a transparent hole, so WantCaptureMouse is false over it
            // AND over any empty dockspace gap — require the cursor to be inside the viewport too.
            const bool overUI = io.WantCaptureMouse || !inViewport(io.MousePos.x, io.MousePos.y);

            // Right mouse enters fly mode (look + WASD/QE), like Unreal's viewport.
            //
            // The cursor is hidden for the duration, as it is in Unreal: while the right button is
            // held the pointer means nothing -- look is a DELTA -- and an arrow sliding around over
            // the scene is just something to watch instead of the scene.
            //
            // Hidden, NOT warped. Re-centring would make the look delta wrong: ImGui measures against
            // the position it last saw, so a warp in between reports the warp as movement and the
            // camera lurches. Suppressing the DRAW is also why this uses ImGui's cursor state rather
            // than Win32's ShowCursor, whose counter has to be paired exactly -- and pairing it to a
            // mouse button being released is precisely how a cursor goes missing for good.
            if (ImGui::IsMouseClicked(1) && !overUI) flying_ = true;
            if (!io.MouseDown[1]) flying_ = false;
            if (flying_) ImGui::SetMouseCursor(ImGuiMouseCursor_None);

            if (flying_) {
                yaw_   += io.MouseDelta.x * lookSpeed_;
                pitch_ -= io.MouseDelta.y * lookSpeed_;
                pitch_ = pitch_ < -1.54f ? -1.54f : (pitch_ > 1.54f ? 1.54f : pitch_);
                if (io.MouseWheel != 0.0f) {
                    flySpeed_ *= (1.0f + io.MouseWheel * 0.15f);
                    flySpeed_ = flySpeed_ < 0.5f ? 0.5f : (flySpeed_ > 400.0f ? 400.0f : flySpeed_);
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
                if (io.MouseWheel != 0.0f) camPos_ += fwd * io.MouseWheel * (flySpeed_ * 0.15f); // dolly
                if (io.MouseDown[2]) { camPos_ -= right * io.MouseDelta.x * 0.02f; camPos_ += up * io.MouseDelta.y * 0.02f; } // MMB pan
            }
            if (!io.WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_F) && sel_ >= 0 && sel_ < (int)objects_.size())
                camPos_ = objects_[sel_].pos - fwd * 6.0f; // focus selection
        }
#endif
#if AVER_MODULE_SCRIPTING
        // Gameplay runs before the frame's render state is composed, so anything a behaviour
        // changes this frame is what gets drawn this frame rather than next. Free and silent when
        // the host declined or when no scripts were found.
        scripts_.update(t.dt);
#endif
#if AVER_MODULE_FRAMEWORK
        // Drive the managed ACTOR tick from the app frame loop, not from the scripting host. The tick
        // GROUPS are a frame-structure concern (they bracket the physics step: PrePhysics -> Physics ->
        // PostPhysics), which the app owns and the generic script host has no business knowing; and
        // aver_fw_tick is a NATIVE framework export, distinct from the bridge's Update above. One call
        // per group per frame, in order — the framework makes exactly one managed tick_all(group) per
        // call, and the bridge walks its own dense list behind it. Harmless every frame with no actors
        // spawned: with no managed dispatch installed, or empty tick buckets, this is a guarded no-op.
        // Who owns the mouse. A running game takes it by default -- an FPS with a visible cursor
        // drifting over the viewport is not playable -- and Shift+F1 gives it back, which is the
        // shortcut Unreal uses and therefore the one people try first. Ending the session always
        // returns it: leaving the cursor hidden after Stop would strand the user in an editor they
        // cannot click.
        // Never in an automated run. --frames and --play-test enter play exactly like a person would,
        // and capturing there would hide and confine the REAL cursor of whoever is at the machine
        // while a headless verification run happens to be going -- a test that reaches out and grabs
        // the mouse is a bad neighbour, and the gate harness launches dozens of these back to back.
        const bool interactive = maxFrames_ == 0 && !playTest_;
        if (e.device()->uiActive() && interactive) {
            const bool wantCapture = playSessionActive() && !releasedByUser_;
            if (ImGui::IsKeyPressed(ImGuiKey_F1, false) && ImGui::GetIO().KeyShift && playSessionActive())
                releasedByUser_ = !releasedByUser_;
            if (!playSessionActive()) releasedByUser_ = false;   // a fresh session starts captured again
            setMouseCaptured(wantCapture && !ImGui::GetIO().WantTextInput);
        }
        pollCapturedMouse();                 // measure and re-centre before the delta is published
        pushInput(e.device()->uiActive());   // publish this frame's keyboard/mouse for gameplay before the tick
        maybeSpawnTestActor();   // one-shot --spawn-test, after scripts have declared their classes
        maybePlayTest();         // one-shot --play-test: begin_play, tick a few frames, end_play
        // Gate the actor tick on PLAYING: in EDITOR gameplay is frozen (like an unopened level), and a
        // PAUSE freezes it without a teardown. The --spawn-test harness is exempt — it drives OnTick
        // directly to prove the tick path without a GameMode. flush() below still runs every frame so an
        // end_play teardown retires regardless of state.
        if (!spawnTestClass_.empty() || aver_fw_play_state() == AVER_FW_PLAY_PLAYING) {
            // The groups BRACKET the step, which is the whole reason they exist as three and not one:
            // PrePhysics is where gameplay says what it wants to happen (a character sets its desired
            // velocity), the step is where the world decides what actually happens, and PostPhysics is
            // where gameplay reads the settled result (where the character ended up, what it hit).
            aver_fw_tick(AVER_FW_TICK_PRE_PHYSICS, t.dt);
#if AVER_MODULE_PHYSICS
            // Stepped with real frame time; the module carries the leftover and runs whole fixed steps
            // internally, so this does NOT make the simulation a function of frame rate.
            aver_phys_step(t.dt);
#endif
            aver_fw_tick(AVER_FW_TICK_PHYSICS, t.dt);
            aver_fw_tick(AVER_FW_TICK_POST_PHYSICS, t.dt);
        }
#endif
#if AVER_MODULE_SCENE
        // Drive the world's frame flush exactly once, AFTER gameplay has spawned/destroyed/moved for the
        // frame and BEFORE onRender walks it: this retires the frame's deferred destroys and propagates
        // world matrices in one linear pass, so the render walk reads settled transforms. Nothing else
        // drives it, and without it a deferred destroy would never reclaim its slot. Gate-neutral: the
        // gate scene populates objects_, not the world, so at gate time the world is empty and flush is
        // a no-op.
        scene::World::instance().flush();
#endif
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
        drivePlayCamera();       // while playing, the view follows the possessed pawn (first/third person)
#endif
        // The geometry path is a DEVICE setting, not a feature's: it decides how every draw reaches
        // the rasteriser. Pushed OUTSIDE the module guard, or a build without Voxi could never
        // select it -- and an unreachable path is an untested one.
#if AVER_MODULE_VOXI
        e.device()->setMeshShaders(voxi::Renderer::get().settings().meshShaders);
#else
        e.device()->setMeshShaders(msOverride_);   // --ms
#endif
#if AVER_MODULE_VOXI
        // Voxi owns the AA setting; push it to the device when it changes (rebuilds targets+PSOs).
        if (voxi::Renderer::get().consumeMsaaDirty())
            e.device()->setSampleCount(static_cast<u32>(voxi::Renderer::get().settings().msaa));

        // Everything the feature owns — GI, the volume's placement in the world, shadows, ray
        // tracing — goes to the feature directly. Routing any of it through IDevice would put
        // feature vocabulary back into the generic interface, which is the whole point of the
        // refactor.
        if (voxiAttached_) {
            const voxi::Settings& vs = voxi::Renderer::get().settings();
            const f32 c[3] = {giCenter_.x, giCenter_.y, giCenter_.z};
            voxiRenderer_.setSettings(vs);
            voxiRenderer_.setVolume(c, giExtent_);
            voxiRenderer_.setDebugView(giDebugView_);
            const Vec3 sd = Vec3{sunAz_, sunAlt_, sunUp_}.getSafeNormal();
            voxiRenderer_.setSun(&sd.x, sunColor_, sunAmbient_);
        }
#endif
        // Confine the scene to the dockspace's central node (latched by buildUI last frame).
        e.device()->setViewportRect((u32)vpX_, (u32)vpY_, (u32)std::fmax(1.0f, vpW_), (u32)std::fmax(1.0f, vpH_));

        const Vec3 fwd = camForward();
        const f32 aspect = viewAspect();
        const Mat4 view = Mat4::lookAtLH(camPos_, camPos_ + fwd, Vec3{0,0,1});
        // Near/far are in WORLD units, and 0.05..5000 was chosen for the placeholder scene at roughly
        // a unit per metre. Under the engine's centimetre contract that is 0.5mm to 50m, so anything
        // past fifty metres is clipped away entirely -- a level only has to be a city block long
        // before it starts disappearing at the far end for no visible reason.
        //
        // Widened only while a level is loaded, so the editor's own scene keeps the depth range it was
        // tuned for and the pixel-exact gates see the projection they recorded. Near stays proportional
        // too: pushing far out without moving near costs depth precision and brings back z-fighting.
        f32 zNear = 0.05f, zFar = 5000.0f;
#if AVER_MODULE_SCENE
        if (!levelEntities_.empty()) { zNear = 2.0f; zFar = 200000.0f; }   // 2cm .. 2km
#endif
        const Mat4 proj = Mat4::perspectiveLH(radians(60.0f), aspect, zNear, zFar);
        const Mat4 viewProj = view * proj;
        const Mat4 invVP = viewProj.inverse();
        e.device()->setCamera(&viewProj.m[0][0], &invVP.m[0][0], &camPos_.x);
        invVP_ = invVP; viewProj_ = viewProj; eye_ = camPos_;

        const Vec3 ld = Vec3{sunAz_, sunAlt_, sunUp_}.getSafeNormal();
        e.device()->setLight(&ld.x, sunColor_, sunAmbient_);
        // Fog density is PER UNIT, and the editor's default is tuned for its own placeholder scene,
        // which is authored at roughly a unit per metre. Gameplay is in CENTIMETRES, so the identical
        // number saturates a few metres out: at the far wall of an 8m room, 1-exp(-0.014 * 1400) is
        // indistinguishable from 1, which is why a correctly built arena rendered as flat white haze.
        // This was the visual "scale mismatch" -- not the geometry, which was right all along.
        //
        // Scaled by the unit ratio while playing rather than changing the default, so the editor's
        // scene keeps the look it was tuned for and the pixel-exact gates (which never enter play)
        // are untouched.
        // A loaded level's own FOG record wins outright: it is authored in the level's units and is
        // the correct value whether or not a session is running. The play-scaled editor default is
        // only the fallback for a world that has no level loaded.
        f32 fog = playSessionActive() ? fogDensity_ * 0.01f : fogDensity_;
#if AVER_MODULE_SCENE
        if (hasLevelFog_) fog = levelFog_;
#endif
        e.device()->setSky(true, skyZenith_, skyHorizon_, fogColor_, fog);
        // Outside the viewport rect is editor chrome, not sky — clear to the dark panel colour.
        e.device()->setClearColor(0.055f, 0.055f, 0.062f, 1);
        // Camera post. Pushed every frame beside the sky because it is the same kind of state, and
        // because the adaptation is a per-frame feedback loop that has to see the current settings.
        e.device()->setPostProcess(post_);
    }

    // Give one actor a material, and move the parameters the material now owns onto it.
    //
    // The shading model computes metallic as gMaterial.x * gMetallicFactor * map, so the authored
    // value can live in EITHER the b1 per-draw block or the b2 material block, and putting it in
    // both would square it. It goes in the material, and b1 is pinned to the identity 1 -- which is
    // the whole point of the step: the Details panel edits a material from here on, not an actor.
    //
    // Base colour deliberately stays on the actor. The voxelisation pass shades with the material
    // system's FALLBACK block, because rhi::IRenderFeature::submitDraw carries no material yet, so
    // neutralising b1's colour would inject white albedo into the radiance volume and turn every
    // bounce white. Base colour moves the day submitDraw carries a material and not before.
#if AVER_MODULE_PBR
    // Turn a material's texture reference into an uploaded GPU texture. Installed on the material
    // system once, at init; without it every slot falls back to the 1x1 identity texture and every
    // surface in the engine is a flat colour, which is exactly what it was before this existed.
    //
    // Static with a `user` pointer because pbr::MaterialSystem::TextureResolver is a plain function
    // pointer: the material system is a Core+RHI target and must not carry a std::function, whose
    // layout is a compiler-and-config-dependent thing to put on a module boundary.
    static rhi::TextureHandle resolveMaterialTexture(const pbr::TextureRef& ref, pbr::TextureSlot slot,
                                                     void* user) {
        auto* self = static_cast<SandboxApp*>(user);
        if (!self || !self->textureFactory_) return 0;

        const std::string path = self->resolveAssetPath(ref);
        if (path.empty()) {
            // An id-only reference with nothing behind it. Named rather than silently ignored: the
            // slot keeps its identity fallback, so the material still renders as a complete surface
            // and the log is the only place the missing binding shows up.
            AVER_WARN("[Material] texture id 0x{:016X} is not in the content index; slot '{}' keeps "
                      "its fallback", ref.id, pbr::MaterialLibrary::textureSlotName(slot));
            return 0;
        }

        // The SLOT decides what the pixels mean, and nothing in an image file does. Getting this
        // wrong is invisible rather than broken -- an sRGB-decoded roughness map is merely a little
        // shinier than authored, everywhere -- which is why it is derived here from the one thing
        // that actually knows, and never guessed from a filename.
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

    // Where an asset reference points on this machine. A path is taken as-is when absolute, and
    // otherwise resolved against the project's content root -- which is what makes an .ocmat
    // portable: it names `Textures/floor_basecolor.png`, not somebody's Documents folder.
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
            // No project, or not under Content: let the path stand and let the decoder report it.
            return p;
        }
        if (ref.id) {
            const auto it = contentIndex_.find(ref.id);
            if (it != contentIndex_.end()) return it->second;
        }
        return {};
    }

    // Index every asset under the project's content root by fnv1a64 of its CONTENT-RELATIVE path, so
    // a `{guid:...}` reference resolves without the file having to be found by name at every use.
    // Rebuilt on project open rather than watched: an editor that rescans a content tree per frame is
    // a disk hit per frame, and a file added mid-session is picked up by reopening the project.
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
            // The id is hashed over the FORWARD-slash spelling, because that is what an .ocmat
            // authored on any platform writes and what Assets.ObjectIdOf hashes on the C# side. A
            // backslash here would make the same file hash differently depending on who wrote it.
            for (char& c : rel) if (c == '\\') c = '/';
            contentIndex_[fnv1a64(std::string_view(rel))] = it->path().string();
        }
        AVER_INFO("[Content] indexed {} asset(s) under {}", contentIndex_.size(), content);
    }

    // The material a level's surface token names, loaded from `<Content>/Materials/<name>.ocmat`.
    // 0 when the project ships no such file, which is not an error: the caller then falls back to
    // the built-in palette, so a blockout with no authored materials still reads as a place.
    pbr::MaterialHandle materialForSurface(const std::string& name) {
        if (name.empty()) return 0;
        const auto cached = materialAssets_.find(name);
        if (cached != materialAssets_.end()) return cached->second;

        pbr::MaterialHandle h = 0;
        const std::string content = project_.contentDir();
        if (!content.empty()) {
            // Two spellings accepted: a bare token (`M_Floor`) that the convention places under
            // Materials/, and an explicit content-relative path for a project that files them
            // elsewhere. Both are one lookup, so neither is the slow path.
            const std::string candidates[2] = {
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
        // Cached even when 0, so a level of a thousand placements naming one absent material costs
        // one stat() rather than a thousand.
        materialAssets_.emplace(name, h);
        return h;
    }

    // Load every material the project ships, up front, and bind each to the surface TOKEN its file
    // name interns to.
    //
    // Up front rather than on demand because a level is not the only thing that names a surface: a
    // gameplay script spawning an actor at run time writes aver_scene_material(0, "M_Target") into
    // its CMeshRenderer, and the render pass only ever sees that i32. Resolving lazily from there is
    // impossible -- there is no way back from the token to the string -- so the mapping has to exist
    // before anything spawns.
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
            // Through materialForSurface so the two paths share one cache and one set of handles:
            // a name loaded here must not be loaded a second time by a level that also names it.
            const pbr::MaterialHandle h = materialForSurface(stem);
            if (!h) continue;
            surfaceMaterials_[aver_scene_material(0, stem.c_str())] = h;
            ++loaded;
        }
        if (loaded) AVER_INFO("[Material] {} project material(s) loaded from {}", loaded, matDir);
#endif
    }

    // Destroy every material the project owns. MaterialSystem::update() sees the handles go invalid
    // and retires their binding sets on its next drain; the TEXTURES behind them stay in its cache,
    // so reopening the same project re-resolves to the same uploads rather than decoding every
    // image again.
    void releaseProjectMaterials() {
        for (const auto& kv : materialAssets_) if (kv.second) pbr::MaterialLibrary::get().destroy(kv.second);
        materialAssets_.clear();
        surfaceMaterials_.clear();
    }
#endif

    void makeMaterialFor(MeshObj& o) {
#if AVER_MODULE_PBR
        pbr::MaterialDesc d;
        d.name            = o.name;
        d.metallicFactor  = o.metallic;
        d.roughnessFactor = o.roughness;
        o.material = pbr::MaterialLibrary::get().create(d);
        if (!o.material) { AVER_WARN("[Sandbox] no material for '{}'; it will draw with the fallback", o.name); return; }
        o.metallic = o.roughness = 1.0f;   // identity in b1; the material carries the authored pair
#else
        (void)o;   // no material system: b1 keeps the authored pair and the frozen path reads it
#endif
    }

    void onRender(Engine& e) override {
        handleManip(e);
        e.device()->setWireframe(wireframe_);
        // The editor's placeholder scene (the grey floor and the orange cube) is EDITOR furniture, not
        // part of anyone's game. It is also authored at a different scale -- roughly a unit per metre,
        // where gameplay is centimetres -- so during a play session it sits inside the level as an
        // 80cm patch of floor with a cube on it, which reads as a bug in the game rather than as the
        // editor's default scene. Hidden while playing; the game builds its own world.
        //
        // Gate-neutral: the oracle runs never enter play, so this is always false at gate time.
        const bool hideEditorScene = playSessionActive();
        for (int i=0;i<(int)objects_.size();++i) {
            MeshObj& o = objects_[i];
            if (!o.visible || hideEditorScene) continue;
            Transform tr; tr.position=o.pos; tr.rotation=quatFromEulerDeg(o.rotDeg); tr.scale=o.scale;
            Mat4 w = tr.toMatrix();
            f32 col[4]={o.color[0],o.color[1],o.color[2],1};
            if (i==sel_) for (int k=0;k<3;++k) col[k]=std::fmin(1.0f,col[k]*1.3f+0.10f);
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
            // The actor's material, applied immediately before the draw it belongs to.
            // setDrawBinding is STICKY and is reset every beginFrame to whatever Voxi registered as
            // the default, so an actor whose material failed to create inherits the fallback rather
            // than the previous actor's -- which is why this is unconditional and not guarded on a
            // non-zero handle.
            if (pbr::MaterialSystem& ms = voxiRenderer_.materials(); ms.ready())
                e.device()->setDrawBinding(ms.bindingSet(o.material), &ms.constants(o.material),
                                           sizeof(pbr::MaterialConstants));
#endif
            e.device()->drawMesh(o.mesh, &w.m[0][0], col, o.metallic, o.roughness);
            if (i == sel_) selectionOutline_ = w, selectionMesh_ = o.mesh, hasSelection_ = true;
        }

        // The selection outline, in Unreal's bright orange-yellow.
        //
        // Drawn as a WIREFRAME pass over the top rather than by tinting the surface: a tint says
        // "this object is a slightly different colour", which is unreadable against a scene that
        // already has colours in it, whereas an edge that follows the silhouette says "this one" at
        // any distance and against any background.
        //
        // Slightly enlarged so the lines sit just outside the surface instead of fighting it for the
        // same depth, which is what would otherwise make the outline stipple and shimmer as the camera
        // moves. The surface draw above is deliberately left exactly as it was, so what a probe pixel
        // in the middle of a face sees does not change.
        // Interactive runs only. The outline is an editor AFFORDANCE, not part of the scene's shading,
        // and the cube primitive is triangulated -- so a wireframe pass draws a diagonal across every
        // face, not merely the silhouette. Enlarged, that diagonal lands in front of the face and
        // straight over the pixel the centre gate probes, so the oracle stops measuring the BRDF and
        // starts measuring the selection highlight. Re-recording would have hidden that rather than
        // fixed it: a shading gate must not be able to pass or fail on editor chrome.
        if (hasSelection_ && !hideEditorScene && maxFrames_ == 0) {
            static constexpr f32 kSelect[4] = {1.0f, 0.62f, 0.12f, 1.0f};   // UE's selection orange
            // The shell is grown by a PROPORTION of its distance from the camera, not by a fixed
            // factor: a constant offset that reads well up close vanishes to sub-pixel across a room,
            // which is exactly where an outline is most needed to find the thing you selected.
            const Vec3 sp{selectionOutline_.m[3][0], selectionOutline_.m[3][1], selectionOutline_.m[3][2]};
            const f32 camDist = (sp - camPos_).size();
            const f32 grow = 1.0f + std::fmin(0.12f, std::fmax(0.02f, camDist * 0.0009f));
            Mat4 o = selectionOutline_;
            for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) o.m[r][c] *= grow;
            e.device()->setWireframe(true);
            e.device()->drawMesh(selectionMesh_, &o.m[0][0], kSelect, 0.0f, 1.0f);
            e.device()->setWireframe(wireframe_);
        }
        hasSelection_ = false;
#if AVER_MODULE_SCENE
        // Scene-entity pass: draw every live entity carrying a CMeshRenderer. This is the bridge from a
        // SPAWNED actor — which lives in the world, not in objects_ above — to the screen. It is additive
        // and gate-neutral by construction: the editor's fixed scene is objects_, and the gate runs spawn
        // no actors, so at gate time the world holds no CMeshRenderer and this loop draws nothing.
        {
            scene::World& w = scene::World::instance();
            int drawn = 0;
            const u32 n = w.count();
            for (u32 i = 0; i < n; ++i) {
                const scene::Entity ent = w.at(i);
                if (w.destroyPending(ent)) continue;   // a deferred-destroyed actor stops drawing at once
                const scene::CMeshRenderer* mr =
                    w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
                if (!mr || !(mr->flags & scene::kMeshRendererVisible) || mr->mesh == 0) continue;
                const auto it = sceneMeshes_.find(mr->mesh);
                if (it == sceneMeshes_.end()) continue; // an unresolved mesh id draws nothing, not garbage
                const Mat4& wm = w.worldMatrix(ent);
                const i32 mat = mr->material;
                // material 0 == "no authored material": the fallback block carries no albedo/spec, so
                // the authored pair rides b1 (a light dielectric, clearly visible). A real material pins
                // b1 to identity and lets its own b2 govern, matching makeMaterialFor's rule for objects_.
                f32 col[4] = {0.80f, 0.80f, 0.85f, 1.0f};
                f32 metallic = 0.0f, roughness = 0.5f;

                // An AUTHORED material, if the project shipped an .ocmat for this surface. 0 means
                // there is none, and 0 is also the handle the material system answers with the
                // fallback for, so one variable covers both cases at every use below.
                // Typed u32 rather than pbr::MaterialHandle so this block still compiles with the
                // material module switched off, which is the whole reason that guard exists.
                u32 authored = 0;
#if AVER_MODULE_PBR
                if (const auto it2 = surfaceMaterials_.find(mat); it2 != surfaceMaterials_.end())
                    authored = it2->second;
#endif
                if (authored) {
                    // b1 pinned to the identity so the material's own block governs outright: the
                    // shading model computes every factor as b1 * b2 * map, so leaving a palette
                    // colour in b1 would tint the authored base colour by it.
                    col[0] = col[1] = col[2] = 1.0f;
                    metallic = roughness = 1.0f;
                } else if (const auto look = surfaceLooks_.find(mat); look != surfaceLooks_.end()) {
                    // The built-in palette, for a surface name with no material asset behind it. It
                    // is what keeps a blockout legible before anything is authored: without it every
                    // spawned actor drew in the same light grey and a whole level merged into one
                    // silhouette with no edges -- floor, walls and crates literally the same colour.
                    col[0] = look->second.col[0]; col[1] = look->second.col[1]; col[2] = look->second.col[2];
                    metallic = look->second.metallic; roughness = look->second.roughness;
                }
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
                // The authored material where there is one, the fallback where there is not. Voxi
                // captures whatever is bound here into its draw list, so the GI bounce is injected
                // from the same surface the lit pass shades -- a textured floor now colours the
                // light it throws back onto the walls.
                if (pbr::MaterialSystem& ms = voxiRenderer_.materials(); ms.ready())
                    e.device()->setDrawBinding(ms.bindingSet(authored), &ms.constants(authored),
                                               sizeof(pbr::MaterialConstants));
#endif
                e.device()->drawMesh(it->second, &wm.m[0][0], col, metallic, roughness);
                ++drawn;
            }
            if (drawn != lastSceneDrawn_) {   // one log line when the count changes, never per frame
                AVER_INFO("[Sandbox] scene-render: {} spawned CMeshRenderer entit{} drawn",
                          drawn, drawn == 1 ? "y" : "ies");
                lastSceneDrawn_ = drawn;
            }
        }
#endif
        e.device()->setWireframe(false); // lines are always solid
        if (showGrid_) {
            // The grid mesh is built as a 40-unit extent with a 2-unit step, which is a 40cm grid of
            // 2cm cells under the engine's centimetre contract -- authored back when the placeholder
            // scene was the only thing in the world and effectively metre-scaled.
            //
            // Scaled up while a LEVEL is loaded, so the floor reference matches the world it is under:
            // 100x gives a 40m grid in 2m cells, which is what makes a correctly-sized 16m room read
            // as a 16m room instead of looking enormous next to a grid a hundred times too fine. This
            // is why the arena looked mis-scaled -- the arena was right and the ruler was wrong.
            //
            // Conditioned on a level being loaded rather than changed outright: the gates draw this
            // grid and compare pixels, and they load no level.
            Mat4 g = Mat4::identity();
#if AVER_MODULE_SCENE
            if (!levelEntities_.empty()) { g.m[0][0] = g.m[1][1] = g.m[2][2] = 100.0f; }
#endif
            e.device()->drawLines(gridMesh_, &g.m[0][0]);
        }
        drawGizmo(e);
        buildUI(e);
        captureCheck(e);
    }

    void onShutdown(Engine& e) override {
        setLogSink(nullptr, nullptr);   // stop mirroring logs before this object goes away
        // ShowCursor is a counter and ClipCursor is global to the desktop: leaving either set would
        // outlive the process and hand the user a machine with an invisible or confined cursor.
        setMouseCaptured(false);
#if AVER_MODULE_PHYSICS
        // Before the rest of teardown: the simulation owns worker threads, and they must be joined
        // while the objects their jobs touch are still alive.
        aver_phys_shutdown();
        groundBody_ = 0;
#endif
#if AVER_WITH_IMGUI
        // The UI descriptor the mark holds is released with the texture, and that pool has no fence
        // of its own -- so the GPU has to be past every frame that drew it first.
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
        // BEFORE the material system goes down with Voxi below. Its shutdown() destroys every
        // texture the resolver handed it, and the resolver's factory pointer is about to become a
        // pointer into a dead device.
        releaseProjectMaterials();
        textureFactory_ = nullptr;
#endif
#if AVER_MODULE_VOXI
        // Deregister before releasing: the device holds a bare pointer to the feature.
        if (voxiAttached_) { e.device()->removeRenderFeature(&voxiRenderer_); voxiAttached_ = false; }
        voxiRenderer_.shutdown();
#else
        (void)e;
#endif
#if AVER_MODULE_SCRIPTING
        // Before the device goes away, so OnShutdown can still touch anything a behaviour was
        // given. Safe after a declined init and safe called twice; ~ScriptHost calls it again.
        scripts_.shutdown();
#endif
        AVER_INFO("[Sandbox] shutdown");
    }
    // --exposure / --bloom / --auto-exposure. Applied before the first frame so a capture run sees
    // the state it asked for rather than one frame of the defaults.
    void setPost(f32 exposure, f32 bloomIntensity, bool autoExposure) {
        post_.exposure = exposure;
        post_.bloomIntensity = bloomIntensity;
        post_.autoExposure = autoExposure;
    }
    void setFocusVoxi(bool b) { focusVoxi_ = b ? 4 : 0; } // --project-settings screenshot aid
    // --drawer screenshot aid: open a drawer from the command line, since a capture run cannot press
    // Ctrl+Space. The slide is snapped past so a short --frames run shows the drawer, not its
    // animation, and `content:<sub>` starts the browser inside a Content subfolder, since a capture
    // run cannot double-click its way there either.
    void setDrawerOpen(int which, std::string sub) {
        if (!which) return;
        drawer_ = drawerShown_ = which == 2 ? Drawer::Log : Drawer::Content;
        drawerAnim_ = 1.0f;
        drawerStartSub_ = std::move(sub);
    }
    void setFocusScript(bool b) { tools_.armNewScript(b); }  // --new-script screenshot aid
    void setFocusTools(bool b) { tools_.armToolsMenu(b); }   // --tools-menu screenshot aid
    void setFocusCompile(bool b) { tools_.armCompile(b); }   // --compile-scripts screenshot aid
    void setFocusReload(int frames) { if (frames > 0) tools_.armReload(frames); } // --reload-scripts [N]
    void setMsaaOverride(int n) { msaaOverride_ = n; }   // --msaa N
    void setGiOverride(int q, bool dbg) { giOverride_ = q; giDebugView_ = dbg; } // --gi / --gi-debug
    void setRtOverride(int q) { rtOverride_ = q; }                              // --rt
    void setMsOverride(bool on) { msOverride_ = on; }                           // --ms
    void setProbe(u32 x, u32 y) { probeX_ = x; probeY_ = y; }                    // --probe X Y
    void setScriptsDir(std::string d) { scriptsDir_ = std::move(d); }            // --scripts <dir>
    void setSpawnTest(std::string cls) { spawnTestClass_ = std::move(cls); }      // --spawn-test <ClassName>
    void setPlayTest() { playTest_ = true; }                                       // --play-test
    void setProjectPath(std::string p) { projectPath_ = std::move(p); }          // <path>.ocproject
    // Arm the start screen. Only ever true for an interactive launch with no project: the
    // verification harness drives the editor with --frames and reads one probe pixel, so a screen
    // in front of the viewport would take out all 13 oracle gates at once.
    void armBrowser(bool on) { browserActive_ = on; }

private:
    // Adopt a project the browser (or the command line) loaded: the title bar and the status bar
    // are the two places the editor claims to have one, so both must actually change.
    void applyProject(Engine& e) {
        project_ = browser_.project();
        if (e.window())
            e.window()->setTitle("Aver Engine \xE2\x80\x94 Editor \xE2\x80\x94 " + project_.name);
#if AVER_MODULE_PBR
        // All three before the start map, and in this order: a level's surfaces resolve to .ocmat
        // assets, those name textures by id, and the id index is what answers them.
        releaseProjectMaterials();
        rebuildContentIndex();
        loadProjectMaterials();
#endif
#if AVER_MODULE_SCENE
        // Open the project's start map, so the editor shows the LEVEL rather than an empty world that
        // only fills in once someone presses Play.
        loadStartMap();
#endif
#if AVER_MODULE_SCRIPTING
        // A project opened from the start screen arrives AFTER the host started, so its scripts
        // have to be picked up here. Guarded on ready(): the command-line path runs this before
        // scripting exists, and resolveScriptsDir() already covers that case — without the guard
        // a project named on the command line would be loaded twice, giving every behaviour in it
        // two instances.
        if (scripts_.ready() && scriptsDir_.empty() && project_.valid()) {
            const std::string bin = editor::scriptsBinaryDir(project_);
            const i32 n = scripts_.loadScripts(bin);
            if (n > 0) AVER_INFO("[Scripting] {} project behaviour(s) live from {}", n, bin);
            else AVER_INFO("[Scripting] no built scripts in {} - use Tools > Reload Scripts", bin);
        }
#endif
    }

#if AVER_MODULE_SCRIPTING
    // Where the CLR host looks for user assemblies, in priority order:
    //   --scripts <dir>       an explicit override, absolute or relative to the executable
    //   <project>\Binaries\Scripts   whatever Tools > Compile Scripts last built
    //   <exe>\Scripts         the engine's own default, which a clean build does not create
    //
    // The override wins so the staged sample stays reachable (`--scripts SampleScripts`) with a
    // project open, and so no oracle gate can ever be made to load a project's scripts by accident.
    std::string resolveScriptsDir() const {
        if (scriptsDir_.empty())
            return project_.valid() ? editor::scriptsBinaryDir(project_) : executableDir() + "\\Scripts";
        const std::string& sd = scriptsDir_;
        const bool absolute = sd.size() > 1 && (sd[1] == ':' || sd[0] == '\\' || sd[0] == '/');
        return absolute ? sd : executableDir() + "\\" + sd;
    }

    // Tools > Reload Scripts, after its `dotnet build` has already succeeded. Runs on the main
    // thread: OnShutdown and OnStart are called from here, and behaviours are a main-thread thing.
    bool reloadScripts(const std::string& binDir, std::string* status) {
        if (!scripts_.ready()) {
            if (status) *status = "The scripting host is not running: " + scripts_.declineReason();
            return false;
        }
        // Unload FIRST. loadScripts is additive, so reloading without a drain would leave the old
        // behaviours live alongside the new ones, both ticking, and the log full of doubles.
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
        return true;
    }
#endif

#if AVER_MODULE_FRAMEWORK
    // ---- HEADLESS TEST TRIGGER (--spawn-test <ClassName>) ---------------------------------------
    // There is no Play button yet, so this is the minimal, clearly-marked way to prove the C# actor
    // loop fires: once, after scripts have loaded (so the bridge has declared the class), find the
    // named class and spawn one. The native spawn dispatches bind -> build_models -> begin_play up
    // into the bridge, and the per-group aver_fw_tick above then drives OnTick each frame. A few frames
    // later it destroys the actor once, so end_play -> unbind runs through the bridge too — the whole
    // lifecycle is observable in the log. NOT a shipping path: it is gated entirely behind a
    // command-line flag the editor never sets itself.
    void maybeSpawnTestActor() {
        if (spawnTestClass_.empty()) return;

        if (!spawnTestDone_) {
            spawnTestDone_ = true;   // spawn is one shot regardless of outcome, so a bad name does not spam
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

        // Tick for a few frames, then destroy once so OnEndPlay is observable too.
        if (spawnTestEntity_ != 0 && ++spawnTestFrames_ == 3) {
            AVER_INFO("[spawn-test] destroying entity {} - watch for its OnEndPlay line", spawnTestEntity_);
            aver_fw_destroy(spawnTestEntity_);
            spawnTestEntity_ = 0;
        }
    }

    // Start a play session from the editor's Play button: find the single user GameMode (and optional
    // GameInstance) by flag and begin_play them. A bare editor with no project scripts has no GameMode,
    // which is why Play looks inert until a script assembly is loaded.
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

    // ---- HEADLESS TEST TRIGGER (--play-test) ----------------------------------------------------
    // The play-lifecycle counterpart of --spawn-test: once a GameMode class has been declared, begin a
    // play session (spawning GameInstance/GameMode/Controller/Pawn), let it tick a few frames, then Stop.
    // The whole GameMode->possessed-Pawn lifecycle is then observable in the log, headless. Flag-gated;
    // the editor never sets it.
    void maybePlayTest() {
        if (!playTest_) return;
        if (!playTestBegun_) {
            if (aver_fw_find_class_with_flags(AVER_FW_CLASS_GAME_MODE) == 0) {
                if (++playTestWait_ > 10) { playTest_ = false;
                    AVER_WARN("[play-test] no GameMode class after 10 frames - pass --scripts <dir> with a GameMode"); }
                return;   // scripts may still be loading; try again next frame
            }
            playTestBegun_ = true;
            AVER_INFO("[play-test] starting - watch for GameMode/Controller/Pawn OnBeginPlay + Pawn OnTick");
            startPlay();
            return;
        }
        if (aver_fw_play_state() == AVER_FW_PLAY_PLAYING) {
            aver_fw_input_set_key(AVER_FW_KEY_W, 1);   // synthetic: hold forward so the possessed character walks
            // Also pull the trigger, and jump once, after the character has had time to land. Holding W
            // only ever proved that movement works; a weapon and a jump that nothing presses are code
            // this harness cannot say anything about, which is the same as untested.
            if (playTestFrames_ > 60) aver_fw_input_set_key(AVER_FW_KEY_MOUSE_LEFT, 1);
            if (playTestFrames_ == 100) aver_fw_input_set_key(AVER_FW_KEY_SPACE, 1);
            // Long enough for the character to fall and settle: it is dropped from 3m, which is about
            // 0.8s of falling, and six frames only ever proved that the tick path fires.
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

    // Publish this frame's keyboard/mouse into the framework so gameplay (the C# Input class) can read it.
    // Called before the actor tick. `uiActive` gates the ImGui reads exactly as the fly-camera block does:
    // headless/no-UI runs have no ImGui input frame, and touching it there hangs — a synthetic --play-test
    // still works because it sets keys AFTER this. Keys/mouse are also suppressed while an editor text field
    // has focus, so a WASD typed into a rename box never walks the character.
    void pushInput(bool uiActive) {
        aver_fw_input_new_frame();
#if AVER_WITH_IMGUI
        if (!uiActive) return;
        // Shift+F1 does not merely show the cursor -- it hands control back to the EDITOR. Publishing
        // nothing leaves every key and button released for the frame, so a character stops walking
        // rather than continuing in whatever direction it was going when the mouse was freed, and
        // clicking on a panel cannot also fire the weapon underneath it. new_frame() above has already
        // cleared the state, so returning here IS "no input this frame".
        if (releasedByUser_ && playSessionActive()) return;
        ImGuiIO& io = ImGui::GetIO();
        const bool kb = !io.WantCaptureKeyboard;
        // The editor's drawer chord wins over gameplay for the keys it uses. Without this the same
        // press does both: Ctrl+Space peeks at the Content Browser AND makes the character jump, and
        // Escape closes the drawer AND opens the game's pause menu. This runs BEFORE buildUI polls the
        // chord, so drawer_ still holds last frame's value -- which is what we want, since it is the
        // press that closes an OPEN drawer that must be swallowed.
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
        // Suppress mouse too while a text field has focus (WantCaptureKeyboard), not only when the cursor is
        // over UI (WantCaptureMouse) — otherwise mouse-look would still turn the character while you type.
        //
        // CAPTURED is the exception to all of that: the game owns the mouse, so no UI can be under the
        // cursor to claim it, and the delta must come from the warp rather than from ImGui. The cursor
        // is re-centred every frame, so ImGui sees an equal-and-opposite jump each time and its own
        // MouseDelta is worse than useless -- it very nearly cancels the movement out.
        const bool m = mouseCaptured_ || (!io.WantCaptureMouse && !io.WantCaptureKeyboard);
        aver_fw_input_set_key(AVER_FW_KEY_MOUSE_LEFT,   m && ImGui::IsMouseDown(0));
        aver_fw_input_set_key(AVER_FW_KEY_MOUSE_RIGHT,  m && ImGui::IsMouseDown(1));
        aver_fw_input_set_key(AVER_FW_KEY_MOUSE_MIDDLE, m && ImGui::IsMouseDown(2));
        if (mouseCaptured_) aver_fw_input_set_mouse(captureDx_, captureDy_, io.MouseWheel);
        else aver_fw_input_set_mouse(m ? io.MouseDelta.x : 0.0f, m ? io.MouseDelta.y : 0.0f, m ? io.MouseWheel : 0.0f);
#endif
    }

    // While playing, position the view camera from the possessed pawn's transform and the character's
    // published view (first- or third-person), overriding the fly camera. Reads the pawn's world matrix:
    // row 3 is the position, row 0 the forward (+X) axis.
    void drivePlayCamera() {
        if (aver_fw_play_state() != AVER_FW_PLAY_PLAYING) return;
        const int32_t pawn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
        if (pawn == 0) return;
        const scene::Entity e = static_cast<scene::Entity>(static_cast<uint32_t>(pawn));
        scene::World& w = scene::World::instance();
        if (!w.valid(e)) return;
        const Mat4& wm = w.worldMatrix(e);
        const Vec3 pawnPos{wm.m[3][0], wm.m[3][1], wm.m[3][2]};
        const Vec3 fwd = Vec3{wm.m[0][0], wm.m[0][1], wm.m[0][2]}.getSafeNormal();
        const Vec3 up{0, 0, 1};

        int32_t mode = AVER_FW_VIEW_THIRD_PERSON; float eye = 160.0f, boom = 450.0f;
        aver_fw_view(&mode, &eye, &boom);

        Vec3 look;
        if (mode == AVER_FW_VIEW_FIRST_PERSON) {
            camPos_ = pawnPos + up * eye;
            look    = fwd;
        } else {
            camPos_ = pawnPos - fwd * boom + up * (boom * 0.55f);
            look    = (pawnPos + up * 90.0f - camPos_).getSafeNormal();
        }
        // camForward() composes {cosP cosY, cosP sinY, sinP}; invert the look direction to yaw/pitch.
        yaw_   = std::atan2(look.y, look.x);
        pitch_ = std::asin(std::fmax(-1.0f, std::fmin(1.0f, look.z)));
    }
#endif

    // Aspect comes from the viewport rect (the dockspace's central node), not the whole window.
    f32 viewAspect() const { return vpH_ > 0.5f ? vpW_ / vpH_ : 1.777f; }
    bool inViewport(f32 mx, f32 my) const { return mx >= vpX_ && mx < vpX_+vpW_ && my >= vpY_ && my < vpY_+vpH_; }
    Vec3 camForward() const {
        return Vec3{ std::cos(pitch_)*std::cos(yaw_), std::cos(pitch_)*std::sin(yaw_), std::sin(pitch_) };
    }
    bool movableSelected() const { return sel_ >= 0 && sel_ < (int)objects_.size(); }

    // Spawn a cube in front of the camera and select it (toolbar Add > Cube).
    void spawnCube(Engine&) {
        if (!cubeMesh_) return;
        MeshObj c; c.mesh = cubeMesh_; c.tris = cubeTris_;
        c.name = "Cube " + std::to_string(++spawnCount_);
        c.pos = camPos_ + camForward() * 8.0f;
        if (snapMove_) for (int k=0;k<3;++k) (&c.pos.x)[k] = snapf((&c.pos.x)[k], moveSnap_);
        c.color[0]=0.72f; c.color[1]=0.72f; c.color[2]=0.74f; c.metallic=0.0f; c.roughness=0.6f;
        objects_.push_back(c);
        sel_ = (int)objects_.size() - 1;
    }
    f32 gizmoLen(const Vec3& origin) const { f32 L = dist(eye_, origin) * 0.17f; return L < 0.5f ? 0.5f : L; }

    // Project a world point to screen pixels (row-vector clip = p * viewProj).
    bool project(const Vec3& wp, f32& sx, f32& sy) const {
        const Mat4& m = viewProj_;
        const f32 x = wp.x*m.m[0][0]+wp.y*m.m[1][0]+wp.z*m.m[2][0]+m.m[3][0];
        const f32 y = wp.x*m.m[0][1]+wp.y*m.m[1][1]+wp.z*m.m[2][1]+m.m[3][1];
        const f32 w = wp.x*m.m[0][3]+wp.y*m.m[1][3]+wp.z*m.m[2][3]+m.m[3][3];
        if (w <= 1e-4f) return false;
        sx = vpX_ + (x / w * 0.5f + 0.5f) * vpW_;          // NDC -> viewport rect, not the window
        sy = vpY_ + (1.0f - (y / w * 0.5f + 0.5f)) * vpH_;
        return true;
    }
    static f32 distToSeg(f32 px, f32 py, f32 ax, f32 ay, f32 bx, f32 by) {
        const f32 vx=bx-ax, vy=by-ay, wx=px-ax, wy=py-ay;
        const f32 len2=vx*vx+vy*vy; f32 t = len2>1e-6f ? (wx*vx+wy*vy)/len2 : 0.0f;
        t = t<0?0:(t>1?1:t); const f32 cx=ax+vx*t, cy=ay+vy*t;
        return std::sqrt((px-cx)*(px-cx)+(py-cy)*(py-cy));
    }

    // Which gizmo handle is under the cursor: 0..2 axis, 3 = centre (screen-plane/uniform),
    // -1 = none. Rotate mode has no centre handle.
    int pickAxis(const Vec3& origin, f32 L, f32 mx, f32 my) const {
        f32 ox, oy; if (!project(origin, ox, oy)) return -1;
        // Generous grab tolerance: the gizmo draws as 1px lines, which are hard to hit
        // precisely on a hi-DPI display, so accept clicks well away from the exact pixel.
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
        // Move / Scale: centre hotspot (screen-plane move / uniform scale), else nearest axis.
        if (std::sqrt((mx-ox)*(mx-ox)+(my-oy)*(my-oy)) < 13.0f*dpi_) return 3;
        int best=-1; f32 bestD=thr;
        for (int a=0;a<3;++a) {
            f32 tx, ty; if (!project(origin + kAxisDir[a]*L, tx, ty)) continue;
            const f32 d=distToSeg(mx,my,ox,oy,tx,ty);
            if (d<bestD) { bestD=d; best=a; }
        }
        return best;
    }

    void applyMove(MeshObj& o, f32 dx, f32 dy) {
        if (activeAxis_ == 3) { // screen-plane move along camera right/up
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
                if (pl2 > 1e-4f) o.pos += A * ((dx*px + dy*py) / pl2); // pixels -> world units along axis
            }
        }
        if (snapMove_) for (int k=0;k<3;++k) (&o.pos.x)[k] = snapf((&o.pos.x)[k], moveSnap_);
    }
    void applyScale(MeshObj& o, f32 dx, f32 dy) {
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
    void applyRotate(MeshObj& o, f32 px, f32 py, f32 mx, f32 my) {
        f32 ox, oy; if (!project(o.pos, ox, oy)) return;
        const f32 a0=std::atan2(py-oy, px-ox), a1=std::atan2(my-oy, mx-ox);
        f32 da=a1-a0; while (da> kPi) da-=kTwoPi; while (da< -kPi) da+=kTwoPi;
        // Rotate so the object follows the cursor around the ring. The sign depends on which
        // way the ring's axis faces the camera (screen y is down => flip accordingly).
        const f32 s = dot(kAxisDir[activeAxis_], camForward()) >= 0.0f ? -1.0f : 1.0f;
        f32& comp = (&o.rotDeg.x)[activeAxis_];
        comp += degrees(da) * s;
        if (snapRot_) comp = snapf(comp, rotSnap_);
    }

    void handleManip(Engine& e) {
#if AVER_WITH_IMGUI
        if (!e.device()->uiActive() || browserActive_) return; // no scene interaction behind the start screen
        const ImGuiIO& io = ImGui::GetIO();

        if (!io.WantCaptureKeyboard) {
            if (ImGui::IsKeyPressed(ImGuiKey_1)) tool_=Tool::Select;
            if (ImGui::IsKeyPressed(ImGuiKey_2)) tool_=Tool::Move;
            if (ImGui::IsKeyPressed(ImGuiKey_3)) tool_=Tool::Rotate;
            if (ImGui::IsKeyPressed(ImGuiKey_4)) tool_=Tool::Scale;
        }
        const f32 mx=io.MousePos.x, my=io.MousePos.y;
        // Only the viewport rect drives the gizmo: the central dock node is a transparent hole,
        // so WantCaptureMouse alone would also let clicks in empty dockspace gaps through.
        const bool overScene = !io.WantCaptureMouse && inViewport(mx, my);

        // Hover highlight when idle over a handle.
        hoverAxis_ = -1;
        if (tool_!=Tool::Select && movableSelected() && !dragging_ && overScene)
            hoverAxis_ = pickAxis(objects_[sel_].pos, gizmoLen(objects_[sel_].pos), mx, my);

        if (ImGui::IsMouseClicked(0) && overScene) {
            int ax = -1;
            if (tool_!=Tool::Select && movableSelected())
                ax = pickAxis(objects_[sel_].pos, gizmoLen(objects_[sel_].pos), mx, my);
            if (ax >= 0) { dragging_=true; activeAxis_=ax; prevMouseX_=mx; prevMouseY_=my; }
            else pick(e, io); // no handle grabbed -> (re)select whatever is under the cursor
        }
        if (!io.MouseDown[0]) { dragging_=false; activeAxis_=-1; }

        if (dragging_ && movableSelected()) {
            MeshObj& o = objects_[sel_];
            const f32 dx=mx-prevMouseX_, dy=my-prevMouseY_;
            if (tool_==Tool::Move)        applyMove(o, dx, dy);
            else if (tool_==Tool::Rotate) applyRotate(o, prevMouseX_, prevMouseY_, mx, my);
            else if (tool_==Tool::Scale)  applyScale(o, dx, dy);
            prevMouseX_=mx; prevMouseY_=my;
        }
#else
        (void)e;
#endif
    }

    void drawGizmo(Engine& e) {
        if (tool_==Tool::Select || !movableSelected()) return;
        const Vec3 O = objects_[sel_].pos;
        const f32 L = gizmoLen(O);
        const Mat4 w = Mat4::scale(Vec3{L,L,L}) * Mat4::translation(O);
        const rhi::LineHandle* nrm = tool_==Tool::Move ? gzMove_ : tool_==Tool::Rotate ? gzRot_ : gzScale_;
        const rhi::LineHandle* hi  = tool_==Tool::Move ? gzMoveHi_ : tool_==Tool::Rotate ? gzRotHi_ : gzScaleHi_;
        e.device()->setLineDepth(false); // draw gizmo on top of geometry
        for (int a=0;a<3;++a) {
            const bool active = (dragging_ && a==activeAxis_) || (!dragging_ && a==hoverAxis_);
            e.device()->drawLines(active ? hi[a] : nrm[a], &w.m[0][0]);
        }
        e.device()->setLineDepth(true);
    }

#if AVER_WITH_IMGUI
    void pick(Engine& e, const ImGuiIO& io) {
        (void)e;
        const f32 nx = (io.MousePos.x - vpX_) / vpW_ * 2.f - 1.f; // NDC within the viewport rect
        const f32 ny = 1.f - (io.MousePos.y - vpY_) / vpH_ * 2.f;
        const Mat4& iv = invVP_;
        const f32 rx = nx*iv.m[0][0]+ny*iv.m[1][0]+iv.m[2][0]+iv.m[3][0];
        const f32 ry = nx*iv.m[0][1]+ny*iv.m[1][1]+iv.m[2][1]+iv.m[3][1];
        const f32 rz = nx*iv.m[0][2]+ny*iv.m[1][2]+iv.m[2][2]+iv.m[3][2];
        const f32 rw = nx*iv.m[0][3]+ny*iv.m[1][3]+iv.m[2][3]+iv.m[3][3];
        const Vec3 farW{rx/rw, ry/rw, rz/rw};
        const Vec3 ro = eye_, rd = farW - eye_;
        int best=-1; f32 bestT=1e30f;
        for (int i=0;i<(int)objects_.size();++i){
            MeshObj& o=objects_[i]; if(!o.visible) continue;
            Transform tr; tr.position=o.pos; tr.rotation=quatFromEulerDeg(o.rotDeg); tr.scale=o.scale;
            const Mat4 iw = tr.toMatrix().inverse();
            const Vec3 lo=xformPoint(iw,ro), ld=xformVec(iw,rd);
            f32 t; if (rayAabb(lo,ld,o.aabbMin,o.aabbMax,t) && t<bestT){ bestT=t; best=i; }
        }
        sel_ = best;
    }

    // A button with a drop-down triangle. The triangle is DRAWN, not typed: the default font
    // only rasterises Latin-1, so glyphs like U+25BE render as '?'.
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

    // A compact vector icon (kind: 0 Select, 1 Move, 2 Rotate, 3 Scale) drawn into a cell.
    void drawToolGlyph(ImDrawList* dl, ImVec2 p, f32 sz, int kind, ImU32 fg) const {
        auto P = [&](f32 fx, f32 fy){ return ImVec2(p.x+fx*sz, p.y+fy*sz); };
        const f32 th = std::fmax(1.6f, 2.0f*dpi_);
        if (kind == 0) { // pointer/cursor
            dl->AddTriangleFilled(P(0.30f,0.20f), P(0.30f,0.70f), P(0.45f,0.56f), fg);
            dl->AddTriangleFilled(P(0.30f,0.20f), P(0.45f,0.56f), P(0.63f,0.49f), fg);
            dl->AddLine(P(0.47f,0.55f), P(0.61f,0.80f), fg, th*1.6f);
        } else if (kind == 1) { // 4-way move
            dl->AddLine(P(0.5f,0.15f), P(0.5f,0.85f), fg, th);
            dl->AddLine(P(0.15f,0.5f), P(0.85f,0.5f), fg, th);
            const f32 a = 0.08f*sz;
            dl->AddTriangleFilled(P(0.5f,0.11f), ImVec2(P(0.5f,0.25f).x-a,P(0.5f,0.25f).y), ImVec2(P(0.5f,0.25f).x+a,P(0.5f,0.25f).y), fg);
            dl->AddTriangleFilled(P(0.5f,0.89f), ImVec2(P(0.5f,0.75f).x-a,P(0.5f,0.75f).y), ImVec2(P(0.5f,0.75f).x+a,P(0.5f,0.75f).y), fg);
            dl->AddTriangleFilled(P(0.11f,0.5f), ImVec2(P(0.25f,0.5f).x,P(0.25f,0.5f).y-a), ImVec2(P(0.25f,0.5f).x,P(0.25f,0.5f).y+a), fg);
            dl->AddTriangleFilled(P(0.89f,0.5f), ImVec2(P(0.75f,0.5f).x,P(0.75f,0.5f).y-a), ImVec2(P(0.75f,0.5f).x,P(0.75f,0.5f).y+a), fg);
        } else if (kind == 2) { // rotate arc + arrowhead
            const ImVec2 c = P(0.5f,0.5f); const f32 r = 0.30f*sz;
            dl->PathArcTo(c, r, -2.30f, 1.15f, 24); dl->PathStroke(fg, 0, th);
            const f32 ea=1.15f; const ImVec2 end(c.x+std::cos(ea)*r, c.y+std::sin(ea)*r); const f32 a=0.07f*sz;
            dl->AddTriangleFilled(ImVec2(end.x-a,end.y-a*0.4f), ImVec2(end.x+a*0.6f,end.y-a), ImVec2(end.x+a*0.2f,end.y+a), fg);
        } else { // scale: diagonal + boxes
            dl->AddLine(P(0.26f,0.74f), P(0.74f,0.26f), fg, th);
            const ImVec2 tl=P(0.74f,0.26f); const f32 b=0.10f*sz;
            dl->AddRectFilled(ImVec2(tl.x-b,tl.y-b), ImVec2(tl.x+b,tl.y+b), fg, 1.5f);
            const ImVec2 br=P(0.26f,0.74f); const f32 b2=0.07f*sz;
            dl->AddRect(ImVec2(br.x-b2,br.y-b2), ImVec2(br.x+b2,br.y+b2), fg, 1.0f, 0, th);
        }
    }
#endif

    void buildUI(Engine& e) {
#if AVER_WITH_IMGUI
        if (!e.device()->uiActive()) return;
        ++frameNo_;   // the Content Browser's directory-cache freshness clock

        // The start screen replaces the editor chrome entirely while it is up.
        if (browserActive_) {
            switch (browser_.draw(dpi_, fontMedium_, logoUiId_, logoAspect_)) {
                case editor::BrowserAction::Open: applyProject(e); browserActive_ = false; break;
                case editor::BrowserAction::Skip: browserActive_ = false; break;
                case editor::BrowserAction::Quit: e.requestExit(); break;
                case editor::BrowserAction::Stay: break;
            }
            return;
        }

        // Drawer shortcuts.
        //
        // Gated on WantCaptureKeyboard, not merely WantTextInput: a modal dialog or a widget being
        // dragged sets the former and not the latter, so a text-only gate lets Ctrl+Space toggle the
        // drawer underneath an open dialog, and lets Escape close the whole drawer mid-drag of its own
        // resize grip. Every other raw key poll in this file already uses the capture flag.
        {
            const ImGuiIO& io = ImGui::GetIO();
            if (!io.WantTextInput && !io.WantCaptureKeyboard) {
                if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false)) toggleDrawer(Drawer::Content);
                // Escape closes the drawer, but only when one is up and nothing is stacked on top of
                // it: Escape over the + Add or Import popup should dismiss that popup, not pull the
                // whole browser out from under it.
                if (drawer_ != Drawer::None && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
                    ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                    drawer_ = Drawer::None;
            }
        }

        // ---------------- menu bar ----------------
        if (ImGui::BeginMainMenuBar()) {
            // Medium weight on the menu bar, matching Unreal's; 0.0f keeps the size already in use.
            if (fontMedium_) ImGui::PushFont(fontMedium_, 0.0f);
            ImGui::TextColored(ImVec4(0.95f,0.42f,0.13f,1),"AE");
            if (ImGui::BeginMenu("File")){
#if AVER_MODULE_SCENE
                const bool haveProject = project_.valid();
                ImGui::BeginDisabled(!haveProject);
                if (ImGui::MenuItem("New Level")) { unloadLevel(); levelName_ = "untitled"; }
                if (ImGui::MenuItem("Open Level")) loadStartMap();
                if (ImGui::MenuItem("Save Level", "Ctrl+S") && !levelPath_.empty()) saveLevel(levelPath_);
                ImGui::EndDisabled();
                if (!haveProject && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Open or create a project first - a level belongs to one.");
#else
                ImGui::MenuItem("New Level"); ImGui::MenuItem("Open Level..."); ImGui::MenuItem("Save Level");
#endif
                ImGui::Separator(); if(ImGui::MenuItem("Exit")) e.requestExit(); ImGui::EndMenu(); }
            if (ImGui::BeginMenu("Edit")){
                ImGui::MenuItem("Undo","Ctrl+Z"); ImGui::MenuItem("Redo","Ctrl+Y"); ImGui::Separator();
                if (ImGui::MenuItem("Editor Preferences...")) showEditorPrefs_ = true;
                if (ImGui::MenuItem("Project Settings...")) showProjectSettings_ = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Window")){
                ImGui::MenuItem("World Outliner"); ImGui::MenuItem("Details");
                // Checked against the drawer state, so the menu reports what is actually up.
                if (ImGui::MenuItem("Content Browser", "Ctrl+Space", drawer_ == Drawer::Content)) toggleDrawer(Drawer::Content);
                if (ImGui::MenuItem("Output Log", nullptr, drawer_ == Drawer::Log)) toggleDrawer(Drawer::Log);
                ImGui::Separator(); if (ImGui::MenuItem("Reset Layout")) dockBuilt_=false; ImGui::EndMenu();
            }
            // Tools sits between Window and Build, where Unreal puts it. It owns its own
            // BeginMenu (see ToolsMenu.cpp) — this file is the frame loop, not a scaffolder.
            tools_.drawMenu(project_);
            if (ImGui::BeginMenu("Build")){ ImGui::MenuItem("Build Lighting"); ImGui::MenuItem("Build Geometry"); ImGui::EndMenu(); }
            if (ImGui::BeginMenu("Select")){ if(ImGui::MenuItem("Select All")) {} if(ImGui::MenuItem("Select None")) sel_=-1; ImGui::EndMenu(); }
            if (ImGui::BeginMenu("Help")){ ImGui::MenuItem("About Aver Engine"); ImGui::EndMenu(); }
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
        ImGui::Button("Save"); ImGui::SameLine();
        if (dropButton("Add")) ImGui::OpenPopup("addActor");
        if (ImGui::BeginPopup("addActor")) {
            ImGui::TextDisabled("Place Actor"); ImGui::Separator();
            if (ImGui::Selectable("Cube"))     spawnCube(e);
            ImGui::Selectable("Sphere",  false, ImGuiSelectableFlags_Disabled);
            ImGui::Selectable("Plane",   false, ImGuiSelectableFlags_Disabled);
            ImGui::Selectable("Point Light", false, ImGuiSelectableFlags_Disabled);
            ImGui::EndPopup();
        }
        ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
        // Compile C#, to the LEFT of the play controls the way UEFN puts Build Verse on the bar: one
        // click rebuilds and hot-swaps the project's scripts, and the icon ON the button says whether
        // disk is built. The play group centres on an absolute position, so this does not shift it.
        tools_.drawCompileButton(project_, dpi_, compileIconUiId_);
        ImGui::SameLine();
        // Play controls, centred like Unreal's.
        {
            const f32 grpW = 200.0f*dpi_;
            ImGui::SameLine(std::fmax(ImGui::GetCursorPosX(), (wsize.x - grpW)*0.5f));
#if AVER_MODULE_FRAMEWORK
            // Play spawns the GameMode session; Stop tears it down; Pause freezes the tick. Play is
            // disabled while playing and Pause/Stop while not — the same shape as Unreal's PIE bar.
            const int32_t ps = aver_fw_play_state();
            const bool playing = ps != AVER_FW_PLAY_EDITOR;
            ImGui::BeginDisabled(playing);
            if (ImGui::Button("Play")) startPlay();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!playing);
            if (ImGui::Button(ps == AVER_FW_PLAY_PAUSED ? "Resume" : "Pause"))
                aver_fw_set_paused(ps != AVER_FW_PLAY_PAUSED ? 1 : 0);
            ImGui::SameLine();
            if (ImGui::Button("Stop")) { aver_fw_end_play(); AVER_INFO("[Sandbox] Stop: play session ended"); }
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
        // Size of the dock region. Must be captured from the host geometry: GetContentRegionAvail()
        // reads 0 after DockSpace() has consumed the region, and DockBuilderSetNodeSize asserts on 0.
        const ImVec2 dockSize(wsize.x, wsize.y - toolbarH - statusH);
        // PassthruCentralNode = the centre is a transparent hole; the 3D scene is scissored into
        // exactly that rect, and mouse input passes through it to the camera/gizmos.
        ImGui::DockSpace(dockId, ImVec2(0,0), ImGuiDockNodeFlags_PassthruCentralNode);

        // Default layout must be built in code: the backend sets io.IniFilename = nullptr, so
        // nothing is ever persisted and panels would otherwise float loose on every launch.
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
            // No bottom split: the Content Browser and Output Log are DRAWERS (drawDrawer), closed on
            // launch and raised over the viewport on demand. They took a quarter of the height
            // permanently before, which is a poor trade for panels you consult in bursts.
            ImGui::DockBuilderFinish(dockId);
        }
        // Latch the central node -> that's the 3D viewport rect (ImGui coords are 1:1 with
        // backbuffer pixels here: DisplaySize is the physical client size, DPI is done via style).
        if (ImGuiDockNode* cn = ImGui::DockBuilderGetCentralNode(dockId)) {
            vpX_ = cn->Pos.x; vpY_ = cn->Pos.y; vpW_ = cn->Size.x; vpH_ = cn->Size.y;
        }
        ImGui::End(); // ##dockhost  (panels are separate windows, so Begin them after this)

        buildPanels(e);
        buildViewportOverlay();
        drawDrawer(e);   // over the viewport, so after the overlay it would otherwise sit behind
        buildEditorPrefs();
        buildProjectSettings();
        tools_.drawModals(project_, dpi_);

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
                    movableSelected() ? objects_[sel_].name.c_str() : "nothing selected");

        // The drawer handles live at the right of the status bar, where Unreal keeps them: the bar is
        // the edge the panels come out of, so it is the edge that should open them.
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
    // The Output Log body: a toolbar (clear / level filter / auto-scroll) over a scrolling, colour-coded
    // view of the captured log. Reads the shared buffer under logMutex_ since logSink fills it off-thread.
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
        // Follow the tail only when the user is already at the bottom, so scrolling up to read stays put.
        if (logAutoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }

    // The roots the Content Browser mounts. "Content" is the project's own; "Engine" exposes the
    // engine's C# classes (AverActor, AverPawn, the Scene maths...) so they can be read from inside the
    // editor instead of hunted down in the source tree -- the same split Unreal draws between a
    // project's content and the engine's.
    //
    // Engine is absent from a shipped build, where engineRoot() finds no source tree. That is correct
    // rather than a failure: there is nothing to browse, and a root that lists nothing is worse than no
    // root at all.
    struct CbRoot { const char* label; std::string path; bool engine; };
    std::vector<CbRoot> cbRoots() const {
        std::vector<CbRoot> r;
        if (project_.valid()) r.push_back({"Content", project_.contentDir(), false});
        if (!cbEngineRoot_.empty()) r.push_back({"Engine", cbEngineRoot_, true});
        return r;
    }

    // ---- Content Browser navigation ------------------------------------------------------------
    // Every folder change goes through here so Back/Forward stay honest. Selecting the folder you are
    // already in is not a navigation and must not push a duplicate entry, or Back becomes a no-op that
    // has to be pressed twice.
    void cbNavigate(const std::string& dir) {
        if (dir.empty() || dir == cbSelectedDir_) return;
        // Going back and then somewhere new FORKS the history: the forward entries described a future
        // that no longer happened, and keeping them would let Forward jump somewhere never visited.
        if (cbHistoryPos_ >= 0 && cbHistoryPos_ + 1 < static_cast<int>(cbHistory_.size()))
            cbHistory_.resize(static_cast<usize>(cbHistoryPos_) + 1);
        cbHistory_.push_back(dir);
        cbHistoryPos_ = static_cast<int>(cbHistory_.size()) - 1;
        cbSelectedDir_ = dir;
        cbSelectedFile_.clear();
        cbFilter_[0] = '\0';   // a search is about the folder you ran it in, not the next one
    }

    bool cbCanBack()    const { return cbHistoryPos_ > 0; }
    bool cbCanForward() const { return cbHistoryPos_ >= 0 &&
                                       cbHistoryPos_ + 1 < static_cast<int>(cbHistory_.size()); }
    // Back/Forward move the cursor WITHOUT touching the list, which is what makes them reversible.
    void cbBack()    { if (cbCanBack())    { cbSelectedDir_ = cbHistory_[static_cast<usize>(--cbHistoryPos_)]; cbSelectedFile_.clear(); } }
    void cbForward() { if (cbCanForward()) { cbSelectedDir_ = cbHistory_[static_cast<usize>(++cbHistoryPos_)]; cbSelectedFile_.clear(); } }

    // The parent, but never above a mounted root -- "up" out of Content into the raw filesystem would
    // leave the browser showing a folder the tree cannot represent.
    std::string cbParentDir() const {
        for (const CbRoot& r : cbRoots())
            if (cbSelectedDir_ == r.path) return {};        // already at a root
        std::error_code ec;
        std::filesystem::path p = std::filesystem::path(cbSelectedDir_).parent_path();
        if (p.empty()) return {};
        const std::string up = p.string();
        for (const CbRoot& r : cbRoots())
            if (up.size() >= r.path.size() && up.compare(0, r.path.size(), r.path) == 0) return up;
        return {};
    }

    // Which extensions an IDE should claim on a double-click. Everything else goes to the shell, so a
    // .png opens in an image viewer rather than as bytes in a code editor.
    static bool cbIsSourceFile(const std::string& ext) {
        static const char* kSource[] = {
            ".cs", ".cpp", ".cxx", ".cc", ".c", ".hpp", ".hxx", ".h", ".inl",
            ".hlsl", ".hlsli", ".glsl", ".json", ".xml", ".csproj", ".txt", ".md", ".ini", ".cmake"};
        for (const char* s : kSource) if (ext == s) return true;
        return false;
    }

    const editor::IdeInfo& cbIde() const {
        const std::vector<editor::IdeInfo>& ides = editor::detectedIdes();
        if (cbIdeChoice_ >= 0 && cbIdeChoice_ < static_cast<int>(ides.size())) return ides[static_cast<usize>(cbIdeChoice_)];
        return editor::preferredIde();
    }

    // What a double-click (or Enter) does: enter a folder, open source in the chosen IDE, hand
    // anything else to the shell.
    void cbOpenEntry(const std::string& full, bool isDir) {
        if (isDir) { cbNavigate(full); return; }
        const std::string ext = lowerExt(std::filesystem::path(full));
        if (cbIsSourceFile(ext)) {
            const editor::IdeInfo& ide = cbIde();
            if (editor::openInIde(ide, full)) { cbStatus_ = "Opened in " + ide.name; return; }
            cbStatus_ = "Could not open in " + ide.name;
            return;
        }
        cbStatus_ = editor::openWithShell(full) ? "Opened" : "Nothing is registered to open that";
    }

    // Drop a folder's cached listing so an edit shows at once. Without this the 20-frame throttle
    // means a rename appears to do nothing for a third of a second, which reads as a failure.
    void cbInvalidate(const std::string& dir) { dirCache_.erase(dir); }

    // Engine content is READ-ONLY through the browser. It is the engine's own source, shared by every
    // project on the machine: renaming AverActor.cs from a game's content browser is never what
    // someone meant to do, and there is no undo for it.
    bool cbIsEditable(const std::string& path) const { return !isEnginePath(path); }

    void cbRenameEntry(const std::string& from, const std::string& newName) {
        if (newName.empty()) return;
        std::error_code ec;
        const std::filesystem::path src(from);
        const std::filesystem::path dst = src.parent_path() / newName;
        if (std::filesystem::exists(dst, ec)) { cbStatus_ = "'" + newName + "' already exists"; return; }
        std::filesystem::rename(src, dst, ec);
        if (ec) { cbStatus_ = "Rename failed: " + ec.message(); return; }
        cbInvalidate(src.parent_path().string());
        // Follow the rename: if the renamed thing was the open folder or the selection, the old path
        // no longer resolves and leaving it selected shows an empty view.
        if (cbSelectedDir_ == from)  { cbSelectedDir_ = dst.string(); }
        if (cbSelectedFile_ == from) { cbSelectedFile_ = dst.string(); }
        cbRewriteHistory(from, dst.string());
        cbStatus_ = "Renamed to " + newName;
    }

    // Keep Back/Forward honest after a folder is renamed or deleted.
    //
    // The history holds PATHS, so a rename leaves entries naming somewhere that no longer exists and
    // Back walks you to a phantom folder -- breadcrumb drawn, tree highlighting nothing, "(this folder
    // is empty)" and no error, because an unreadable directory and an empty one look identical here.
    // `to` empty means the path is gone: drop those entries instead of rewriting them.
    //
    // Subfolders are rewritten too: renaming a folder moves everything beneath it.
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
                if (to.empty()) { if (h == current) newCurrent.clear(); continue; }   // gone
                next = to + h.substr(from.size());
            }
            if (h == current) newCurrent = next;
            // Collapse the duplicate a rewrite can create when two entries fold onto one path.
            if (kept.empty() || kept.back() != next) kept.push_back(next);
        }
        cbHistory_.swap(kept);
        // Re-point the cursor at what the user was actually looking at.
        cbHistoryPos_ = -1;
        for (usize i = 0; i < cbHistory_.size(); ++i)
            if (cbHistory_[i] == newCurrent) { cbHistoryPos_ = static_cast<int>(i); break; }
        if (cbHistoryPos_ < 0 && !cbHistory_.empty()) cbHistoryPos_ = static_cast<int>(cbHistory_.size()) - 1;
    }

    void cbDuplicateEntry(const std::string& path) {
        std::error_code ec;
        const std::filesystem::path src(path);
        const std::string stem = src.stem().string(), ext = src.extension().string();
        // Find a free "<name>2", "<name>3"... rather than overwriting anything.
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

    void cbDeleteEntry(const std::string& path) {
        const std::filesystem::path src(path);
        const std::string parent = src.parent_path().string();
        if (!editor::moveToRecycleBin(path)) { cbStatus_ = "Could not delete " + src.filename().string(); return; }
        cbInvalidate(parent);
        if (cbSelectedFile_ == path) cbSelectedFile_.clear();
        // Deleting the folder you are standing in has to move you somewhere that still exists...
        if (cbSelectedDir_ == path) cbSelectedDir_ = parent;
        // ...and so does everywhere Back could take you.
        cbRewriteHistory(path, std::string());
        cbStatus_ = "Moved " + src.filename().string() + " to the recycle bin";
    }

    void cbCreateFolder(const std::string& parent, const std::string& name) {
        if (name.empty()) return;
        // Enforced HERE as well as at the menu items, because a guard that lives only at call sites is
        // one forgotten call site away from being absent -- which is exactly how the + Add menu ended
        // up able to write into the engine's own source tree.
        if (!cbIsEditable(parent)) { cbStatus_ = "Engine content is read-only"; return; }
        std::error_code ec;
        const std::filesystem::path dst = std::filesystem::path(parent) / name;
        if (std::filesystem::exists(dst, ec)) { cbStatus_ = "'" + name + "' already exists"; return; }
        std::filesystem::create_directory(dst, ec);
        if (ec) { cbStatus_ = "Could not create folder: " + ec.message(); return; }
        cbInvalidate(parent);
        cbStatus_ = "Created " + name;
    }

    // The right-click menu for one entry. Mirrors the verbs Unreal's browser offers, minus the ones
    // that need an asset database (Reference Viewer, Migrate) rather than a filesystem.
    void cbItemContextMenu(const std::string& full, const std::string& name, bool isDir) {
        if (!ImGui::BeginPopupContextItem("##cbitemctx")) return;
        const bool editable = cbIsEditable(full);
        ImGui::TextDisabled("%s", name.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem(isDir ? "Open" : "Open in editor", "Double-click")) cbOpenEntry(full, isDir);
        if (!isDir) {
            // "Open With" names each IDE, so the click says what will actually happen rather than
            // trusting the user to know what the default resolved to.
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
        // DEFERRED, like rename and delete, and for a harder reason than tidiness: this menu is
        // submitted from INSIDE the file view's clipper loop, which is iterating raw DirEntry pointers
        // into dirCache_. Duplicating invalidates that cache entry, destroying the vector and every
        // string in it, and the loop then reads the freed memory on the very next line. Running it
        // after the view has finished is what makes the pointers outlive the frame that uses them.
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

    // The Content Browser body: an Add menu (the creation flows, shared with Tools), an Import button, and
    // a folder tree + file view scanned LIVE from the mounted roots.
    void drawContentBrowser() {
        const std::vector<CbRoot> roots = cbRoots();
        if (roots.empty()) {
            ImGui::TextDisabled("No project loaded - nothing is mounted.");
            ImGui::TextDisabled("Create or open a project (File menu) to browse its Content folder.");
            return;
        }
        // Land on the project's own Content folder rather than nowhere, so the browser opens showing
        // the thing the user is working on. Resolved on the first drawn frame, not at parse time:
        // contentDir() does not exist until a project has been applied.
        if (cbSelectedDir_.empty()) cbNavigate(roots.front().path);
        if (!drawerStartSub_.empty()) {
            // Resolve through the filesystem so separators and case match the strings the tree builds
            // from directory_iterator -- a raw join leaves "Content\Scripts/AI", which enumerates fine
            // but never compares equal, so the tree highlight silently never matches.
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path target = fs::canonical(fs::path(roots.front().path) / drawerStartSub_, ec);
            if (ec) AVER_WARN("[Sandbox] --drawer content:{}: no such folder under Content", drawerStartSub_);
            else    cbNavigate(target.string());
            drawerStartSub_.clear();
        }
        // Back / Forward / Up, in the order and place every file manager puts them.
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
            // Same read-only guard the BACKGROUND menu has. Engine content is shared source; a New
            // Folder or an Import into it is never what someone meant, and having the guard on only
            // one of the two routes to the same operation is the same as not having it.
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

        // View controls, right-aligned: a two-state segmented control, and the tile zoom when tiles are
        // showing. Both states are drawn as buttons with the active one held down, rather than one
        // button labelled with the view you would switch TO -- that reads as a label, not a state.
        {
            auto viewTab = [&](const char* label, bool active) {
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                const bool hit = ImGui::Button(label);
                if (active) ImGui::PopStyleColor();
                return hit;
            };
            // Search first, so it keeps a stable place as the view controls change width.
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

        // Leave room for the footer, which reports the count and the last operation's outcome -- a
        // file operation that says nothing is indistinguishable from one that silently failed.
        const f32 footerH = ImGui::GetTextLineHeightWithSpacing() + 6.0f*dpi_;
        ImGui::BeginChild("cbTree", ImVec2(220.0f * dpi_, -footerH), true);
        for (const CbRoot& r : roots) {
            ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_SpanAvailWidth;
            if (!r.engine) rootFlags |= ImGuiTreeNodeFlags_DefaultOpen;   // the project's own opens; the engine's stays furled
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
        // Right-click on empty space gets the FOLDER's verbs, not an entry's. NoOpenOverItems keeps it
        // from stealing the right-click an entry already handles.
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

    // Count what is on screen, name what is selected, and report the last operation.
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

    // F2 / Delete / Enter / Ctrl+D on the selection, the accelerators the context menu advertises.
    //
    // The gate is deliberately three things, because a focus-plus-text-input gate is not enough:
    //
    //  * A MODAL of this panel's own still counts as "the browser is focused" -- IsWindowFocused with
    //    RootAndChildWindows walks the popup hierarchy, and the modal's parent in the begin stack is
    //    the drawer. So without the popup test, pressing Del while the DELETE CONFIRMATION is open
    //    re-arms the dialog against whatever is merely SELECTED, silently retargeting the pending
    //    delete at a different file -- and right-clicking does not change the selection, so the two
    //    differ exactly when a user is most likely to reach for the key.
    //  * WantCaptureKeyboard, not just WantTextInput: a modal without a text field sets the former
    //    and not the latter. The drawer's own Ctrl+Space handler already learned this.
    //  * Without both, Ctrl+D executed a filesystem write while a blocking confirmation was on screen.
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

    // Rename / delete / new-folder, opened from the deferred flags. They live at the PANEL level
    // rather than inside the file view because an ImGui popup id belongs to the window that opens it,
    // and the request comes from a child.
    void cbFileOpModals() {
        // Duplicate needs no dialog, but it DOES need to happen here rather than where it was asked
        // for: the request comes from a context menu submitted inside the file view's clipper loop,
        // which is iterating pointers into the very cache entry this invalidates.
        if (cbWantDuplicate_) { cbWantDuplicate_ = false; cbDuplicateEntry(cbContextPath_); }

        if (cbWantRename_)    { ImGui::OpenPopup("cbRename");    cbWantRename_ = false; }
        if (cbWantDelete_)    { ImGui::OpenPopup("cbDelete");    cbWantDelete_ = false; }
        if (cbWantNewFolder_) { ImGui::OpenPopup("cbNewFolder"); cbWantNewFolder_ = false; }
        // Import is opened here for the ID-scoping reason the others are: an ImGui popup id belongs to
        // the window that opens it, so OpenPopup called from inside the background context menu could
        // never match the BeginPopup that drawImportModal does at panel level -- the menu item simply
        // did nothing, silently.
        if (cbWantImport_)    { ImGui::OpenPopup("cbImport");    cbWantImport_ = false; }

        if (ImGui::BeginPopupModal("cbRename", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextDisabled("Rename %s", cbContextIsDir_ ? "folder" : "file");
            ImGui::SetNextItemWidth(360.0f*dpi_);
            // Focus the field on the frame the modal appears, so a rename is type-then-Enter with no
            // click in between -- which is what F2 means everywhere else.
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
            // Say where it goes. "Delete" that means "recoverable" is worth stating, because the user's
            // decision is different if it does not.
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

    // The breadcrumb: the selected folder as clickable ancestors, so going back up is one click rather
    // than a hunt through the tree. Segments are rebuilt from the root prefix so what is clicked is
    // byte-identical to what the tree stores.
    void drawBreadcrumb(const std::string& dir) {
        const std::vector<CbRoot> roots = cbRoots();
        const CbRoot* owner = nullptr;
        for (const CbRoot& r : roots)
            if (dir.size() >= r.path.size() && dir.compare(0, r.path.size(), r.path) == 0) { owner = &r; break; }
        if (!owner) { ImGui::TextDisabled("%s", dir.c_str()); return; }

        std::string acc = owner->path;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f*dpi_, 1.0f*dpi_));
        if (ImGui::SmallButton(owner->label)) cbNavigate(acc);
        // The tail after the root, split on either separator -- a path may carry both.
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

    // Recurse the folder tree, scanning only the branches the user has opened (TreeNodeEx is lazy), so a
    // deep project costs nothing until it is expanded. A click on a folder selects it for the file list.
    // Enumerate a directory SAFELY and THROTTLED. Safely: driven by the non-throwing increment(ec) — a
    // filesystem_error escaping through the ImGui frame is std::terminate (the same hazard IdeIntegration
    // guards). Throttled: cached for a short window so a large or network folder is not re-walked every
    // frame. dirs/files come back sorted so the listing is stable frame to frame.
    const DirListing& dirListing(const std::string& dir) {
        DirListing& c = dirCache_[dir];
        if (frameNo_ - c.stamp < 20) return c;   // fresh enough
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
                    // A folder wears the Module icon when it is engine content, or when it is a C++
                    // module in its own right -- both are "part of the engine's machinery" rather than
                    // a plain bag of assets.
                    std::error_code mec;
                    ent.module = isEnginePath(ent.full) ||
                                 std::filesystem::exists(ent.path / "CMakeLists.txt", mec);
                    ++c.dirCount;
                } else {
                    ent.tile = fileIconTile(ent.full, ent.name, lowerExt(ent.path));
                }
                c.entries.push_back(std::move(ent));
            }
        } catch (const std::exception&) { /* keep what we read; a mid-walk failure is not fatal to the UI */ }
        // Folders first, then files, each alphabetical -- the order every file manager uses, and the
        // one the views rely on to split the two groups by dirCount alone.
        std::sort(c.entries.begin(), c.entries.end(), [](const DirEntry& a, const DirEntry& b) {
            if (a.isDir != b.isDir) return a.isDir;
            return a.full < b.full;
        });
        return c;
    }

    void drawFolderTree(const std::string& dir) {
        // Copy the folder paths out before drawing: a click below reassigns cbSelectedDir_ and can
        // refresh the cache, so we must not still be iterating the listing we came from.
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

    // Which sprite tile a file gets (0 C# Script, 1 C# Class, 2 C++ Class, 3 C++ Module), or -1 for none.
    // A .cs is classified once — script (has a lifecycle base / [AverClass]) vs plain class — by peeking at
    // the file HEAD (markers only appear near the top) and cached, so nothing re-reads a file per frame or
    // slurps a huge one whole.
    int fileIconTile(const std::string& path, const std::string& name, const std::string& ext) {
        if (name == "CMakeLists.txt") return 3;   // a module's marker
        if (ext == ".cpp" || ext == ".cxx" || ext == ".cc" || ext == ".hpp" || ext == ".hxx" || ext == ".h")
            return 2;   // C++ Class
        if (ext != ".cs") return -1;
        // Keyed by modification time, not by path alone: a file edited to derive AverActor mid-session
        // must lose the plain-class icon. A path-only cache pins the first answer until restart, which
        // is exactly the icon the gallery now uses as the file's primary identity.
        std::error_code ec;
        const auto mtime = std::filesystem::last_write_time(path, ec);
        if (auto it = fileIconCache_.find(path);
            it != fileIconCache_.end() && !ec && it->second.first == mtime) return it->second.second;

        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return 1;            // unreadable: guess, but never cache the guess
        int tile = 1;                           // default: a plain C# Class
        char head[8192];
        in.read(head, sizeof(head));            // the head only — a marker past 8 KB is not a class declaration
        const std::string body(head, static_cast<size_t>(in.gcount()));
        static const char* kScriptMarkers[] = {
            "AverBehaviour", "AverActor", "AverPawn", "AverCharacter", "AverPlayerController",
            "AverGameMode", "AverGameInstance", "[AverClass", "[AverGameMode"};
        for (const char* m : kScriptMarkers) if (body.find(m) != std::string::npos) { tile = 0; break; }
        if (!ec) fileIconCache_[path] = {mtime, tile};
        return tile;
    }

    // A folder, drawn rather than sprited: the supplied icon sheet covers file TYPES, and a folder is
    // not one of them. A body with a raised tab, which is the shape everyone already reads as "folder".
    static void folderGlyph(ImDrawList* dl, ImVec2 c, f32 s, ImU32 col) {
        const f32 w = s, h = s * 0.76f;
        const f32 x0 = c.x - w*0.5f, y0 = c.y - h*0.5f, tabH = h * 0.17f;
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w*0.44f, y0 + tabH*2.4f), col, s*0.06f);
        dl->AddRectFilled(ImVec2(x0, y0 + tabH), ImVec2(x0 + w, y0 + h), col, s*0.07f);
    }

    // A generic document, for a file the sheet has no tile for -- a page with its corner turned, so an
    // unrecognised file still reads as a file rather than as a missing icon.
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

    // Blit one tile of an N-tile sheet, fitted INSIDE an s-by-s box at its own aspect so a landscape
    // folder and a portrait document occupy the same visual slot without either being stretched.
    static void blitTile(ImDrawList* dl, u64 tex, ImVec2 centre, f32 s, f32 aspect, int tile, int tiles) {
        const f32 w = aspect >= 1.0f ? s : s * aspect;
        const f32 h = aspect >= 1.0f ? s / aspect : s;
        dl->AddImage(static_cast<ImTextureID>(tex),
                     ImVec2(centre.x - w*0.5f, centre.y - h*0.5f),
                     ImVec2(centre.x + w*0.5f, centre.y + h*0.5f),
                     ImVec2(static_cast<f32>(tile) / tiles, 0.0f),
                     ImVec2(static_cast<f32>(tile + 1) / tiles, 1.0f));
    }

    // One entry's icon, for whichever view is up, so the two cannot drift apart. Every sheet is
    // optional: a missing one falls back to the drawn glyph rather than to a blank cell.
    void drawEntryIcon(ImDrawList* dl, ImVec2 centre, f32 s, bool isDir, int tile, bool module) {
        if (isDir) {
            if (folderIconsUiId_) blitTile(dl, folderIconsUiId_, centre, s, folderIconAspect_, module ? 1 : 0, 2);
            else                  folderGlyph(dl, centre, s, IM_COL32(232, 187, 92, 255));
            return;
        }
        if (tile >= 0 && fileIconsUiId_) { blitTile(dl, fileIconsUiId_, centre, s, fileIconAspect_, tile, 4); return; }
        fileGlyph(dl, centre, s, IM_COL32(150, 154, 162, 255));
    }

    // Lower-cased extension, since the classifier compares against lower-case literals and Windows will
    // happily hand back ".CS".
    static std::string lowerExt(const std::filesystem::path& p) {
        std::string ext = p.extension().string();
        for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return ext;
    }

    // Case-insensitive substring, for the search box. Small and local: pulling in a locale-aware
    // comparison for a filename filter would be a lot of machinery for "does this name contain that".
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

    // Trim a name to at most `lines` wrapped lines, ending in an ellipsis when it does not fit.
    // Letting the clip rect cut it instead leaves a half-drawn glyph, which reads as a rendering fault
    // rather than as "there is more name here" -- and these are dotted namespace names, so the part
    // that gets cut is exactly the part that distinguishes them.
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

    // The gallery: a wrapped grid of icon tiles, the view a content browser is normally read in --
    // you recognise a script or a module by its icon far faster than by finding its name in a column.
    void drawFolderGallery(const std::vector<const DirEntry*>& shown) {
        const f32 tile   = cbTileSize_ * dpi_;
        const f32 pad    = 8.0f * dpi_;
        const f32 labelH = ImGui::GetTextLineHeight() * 2.0f + 4.0f * dpi_;   // two lines: names wrap
        const f32 cellW  = tile, cellH = tile + labelH;
        int perRow = static_cast<int>((ImGui::GetContentRegionAvail().x + pad) / (cellW + pad));
        if (perRow < 1) perRow = 1;   // a panel narrower than one tile still gets one per row

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const int rows = (static_cast<int>(shown.size()) + perRow - 1) / perRow;
        // Clip by ROW: a project folder can hold thousands of files, and submitting a cell for every
        // one of them every frame costs more than everything else the editor draws put together.
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
                        // Double-click OPENS, whatever it is: a folder is entered, a source file goes
                        // to the chosen IDE, anything else to the shell.
                        if (ImGui::IsMouseDoubleClicked(0) || (e.isDir && !cbDoubleClickEnter_))
                            cbOpenEntry(e.full, e.isDir);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.name.c_str());
                    cbItemContextMenu(e.full, e.name, e.isDir);
                    drawEntryIcon(dl, ImVec2(o.x + cellW*0.5f, o.y + tile*0.5f), tile*0.52f, e.isDir, e.tile, e.module);
                    // Centre a name that fits on one line; let a longer one wrap from the left and clip
                    // at the cell, so a long class name degrades instead of running into its neighbour.
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

    void drawFolderFiles(std::string dir) {   // by value: a click below reassigns cbSelectedDir_
        drawBreadcrumb(dir);
        ImGui::Separator();

        // Pointers into the cached listing rather than copies of it. Nothing mutates dirCache_ during
        // the draw -- a click only changes which key is looked up NEXT frame -- and copying two vectors
        // of paths per frame was the panel's largest single cost.
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

        // The list: one row per entry, same icons, for when the names are what you are scanning.
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

    // The Import modal: type a source file, and Import COPIES it into the selected folder — a real, if
    // minimal, importer (a converting pipeline, FBX/PNG -> engine formats, is a later stage). Logs the
    // outcome to the Output Log.
    void drawImportModal() {
        if (!ImGui::BeginPopup("cbImport")) return;
        ImGui::TextUnformatted("Import an asset by copying it into the selected folder.");
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

    void importAsset(const std::string& src, const std::string& destDir) {
        std::error_code ec;
        // The engine's source tree is not an import target, whichever button got you here.
        if (!cbIsEditable(destDir)) {
            AVER_WARN("[Import] '{}' is engine content and is read-only", destDir);
            cbStatus_ = "Engine content is read-only";
            return;
        }
        if (!std::filesystem::exists(src, ec)) { AVER_WARN("[Import] source not found: {}", src); return; }
        const std::string name = std::filesystem::path(src).filename().string();
        const std::string dest = destDir + "\\" + name;
        // Refuse rather than overwrite: silently destroying an existing same-named asset is data loss, and
        // the only feedback would be a success line. The user renames the source or clears the target first.
        if (std::filesystem::exists(dest, ec)) {
            AVER_WARN("[Import] '{}' already exists in {} - not overwritten; rename the source or remove it first", name, destDir);
            return;
        }
        std::filesystem::copy_file(src, dest, ec);
        if (ec) AVER_WARN("[Import] failed to copy '{}' -> '{}': {}", src, dest, ec.message());
        else    AVER_INFO("[Import] imported '{}' into {}", name, destDir);
    }

    // Docked panels. These are plain windows — the dock builder placed them, and the user can
    // re-dock, tab or float them freely from here on.
    // The material half of the Details panel. Every control here edits a pbr::MaterialDesc
    // through the library, NOT the actor: an actor references a material, and two actors
    // sharing one would otherwise be edited independently and diverge with no way to tell from
    // the outliner. touch() marks it dirty; the material system drains that in Voxi's prePass.
    //
    // Without the module this falls back to editing the actor, because the frozen no-material
    // path reads b1 directly and there is nothing else for a slider to write to.
    void materialPanel(MeshObj& o) {
#if AVER_MODULE_PBR
        pbr::MaterialDesc* d = pbr::MaterialLibrary::get().mutableDesc(o.material);
        if (!d) { ImGui::TextDisabled("No material (drawing with the fallback)"); return; }
        bool changed = false;
        changed |= ImGui::SliderFloat("Metallic", &d->metallicFactor, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("Roughness", &d->roughnessFactor, 0.045f, 1.0f);
        changed |= ImGui::SliderFloat("Normal Scale", &d->normalScale, 0.0f, 4.0f);
        changed |= ImGui::SliderFloat("Occlusion", &d->occlusionStrength, 0.0f, 1.0f);
        // Reflectance and f90 are the two the material system exists to make authorable; the
        // range covers water (~0.02) through gemstone (~0.17), which is why it stops at 0.2
        // rather than at 1 where every useful value would sit in the first fifth of the slider.
        changed |= ImGui::SliderFloat("Reflectance", &d->reflectance, 0.0f, 0.2f, "%.3f");
        changed |= ImGui::SliderFloat("Grazing (f90)", &d->f90, 0.0f, 1.0f);
        changed |= ImGui::DragFloat3("Emissive", d->emissiveFactor, 0.01f, 0.0f, 32.0f);

        ImGui::Separator();
        // How the maps below are laid onto the surface. World-aligned is the one a blockout wants:
        // every box in a level shares one unit cube's 0..1 UVs, so mesh UVs stretch a single tile
        // across a sixteen-metre floor and cram the same tile into a fifty-centimetre crate.
        int uvMode = static_cast<int>(d->uvMode);
        if (ImGui::Combo("UV Mapping", &uvMode, "Mesh UVs\0World Aligned\0")) {
            d->uvMode = static_cast<pbr::UvMode>(uvMode);
            changed = true;
        }
        if (d->uvMode == pbr::UvMode::WorldAligned) {
            // Logarithmic, because the useful range spans a 10 cm decal to a 10 m floor slab and a
            // linear slider would spend nine tenths of its travel above two metres.
            changed |= ImGui::SliderFloat("Tile Size (cm)", &d->uvTiling, 5.0f, 2000.0f, "%.0f",
                                          ImGuiSliderFlags_Logarithmic);
        }

        ImGui::Separator();
        // One path field per slot. A path is all the material system wants: it holds the
        // reference and never resolves it, because resolving needs the asset system a tier up.
        // Typing a path that does not resolve leaves the slot on its identity fallback, so the
        // surface stays complete rather than turning black.
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
        // mutableDesc() hands out a raw pointer and does NOT mark anything, so an edit that
        // forgot this would show in the panel and never reach the GPU -- the exact failure the
        // library's comment warns about.
        if (changed) pbr::MaterialLibrary::get().touch(o.material);
#else
        ImGui::SliderFloat("Metallic", &o.metallic, 0.0f, 1.0f);
        ImGui::SliderFloat("Roughness", &o.roughness, 0.02f, 1.0f);
#endif
    }

    void buildPanels(Engine& e) {
        (void)e;   // the panels read app/project state, not the device, since the Output Log/Content Browser landed
        ImGui::Begin("World Outliner");
        for (int i=0;i<(int)objects_.size();++i)
            if (ImGui::Selectable((std::string("  ")+objects_[i].name).c_str(), sel_==i)) sel_=i;
        ImGui::Separator();
        if (ImGui::Selectable("  Directional Light (Sun)", sel_==-2)) sel_=-2;
        if (ImGui::Selectable("  Sky + Atmosphere", sel_==-3)) sel_=-3;
        if (ImGui::Selectable("  Post Process", sel_==-4)) sel_=-4;
        ImGui::End();

        ImGui::Begin("Details");
        if (sel_>=0 && sel_<(int)objects_.size()){
            MeshObj& o=objects_[sel_]; ImGui::TextUnformatted(o.name.c_str()); ImGui::Separator();
            if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::DragFloat3("Location", &o.pos.x, 0.05f);
                ImGui::DragFloat3("Rotation", &o.rotDeg.x, 1.0f);
                ImGui::DragFloat3("Scale", &o.scale.x, 0.01f, 0.02f, 100.f);
            }
            if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::ColorEdit3("Base Color", o.color);
                materialPanel(o);
            }
            ImGui::Checkbox("Visible", &o.visible);
        } else if (sel_==-2){
            ImGui::TextUnformatted("Directional Light"); ImGui::Separator();
            ImGui::SliderFloat("Azimuth", &sunAz_, -1, 1); ImGui::SliderFloat("Altitude", &sunAlt_, -1, 1); ImGui::SliderFloat("Up", &sunUp_, 0.05f, 2);
            ImGui::ColorEdit3("Color", sunColor_); ImGui::SliderFloat("Ambient", &sunAmbient_, 0, 1);
        } else if (sel_==-3){
            ImGui::TextUnformatted("Sky + Atmosphere"); ImGui::Separator();
            ImGui::ColorEdit3("Zenith", skyZenith_); ImGui::ColorEdit3("Horizon", skyHorizon_);
            ImGui::ColorEdit3("Fog", fogColor_); ImGui::SliderFloat("Fog density", &fogDensity_, 0, 0.06f, "%.4f");
        } else if (sel_==-4){
            // The camera's post chain. Everything here defaults to the identity, so an untouched
            // editor renders exactly what it did before the chain existed -- which is what the
            // pixel-exact gates measure, and why the defaults are not a matter of taste.
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
            // Zero is OFF, not "on and invisible": the pyramid is not built at all, so the whole
            // chain costs one fullscreen pass. Worth saying in the tooltip, because a slider at zero
            // usually still costs what it costs at one.
            ImGui::SliderFloat("Bloom", &post_.bloomIntensity, 0.0f, 1.0f, "%.3f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Zero skips the whole bloom pyramid, not just its weight");
            if (post_.bloomIntensity > 0.0f) {
                ImGui::SliderFloat("Threshold", &post_.bloomThreshold, 0.0f, 8.0f, "%.2f");
                ImGui::SliderFloat("Knee", &post_.bloomKnee, 0.0f, 2.0f, "%.2f");
            }
        } else ImGui::TextDisabled("Select an actor in the World Outliner");
        ImGui::End();

    }

    // The bottom drawer: the Content Browser and the Output Log, which slide up over the viewport
    // instead of holding a dock node open all session. Closed is the resting state, so the editor
    // starts with the whole height given to the scene.
    void drawDrawer(Engine& e) {
        // Ease the slide, frame-rate independently. dt is clamped because a hitch (a shader compile,
        // a project load) must not teleport the panel: the drawer should look the same on a stalled
        // frame as on a fast one.
        const f32 dt = std::fmin(e.time().dt, 0.05f);
        const f32 target = drawer_ == Drawer::None ? 0.0f : 1.0f;
        drawerAnim_ += (target - drawerAnim_) * (1.0f - std::exp(-drawerRate_ * dt));
        if (drawer_ != Drawer::None) drawerShown_ = drawer_;
        // Fully retracted: draw nothing at all, so a closed drawer costs no window and no directory walk.
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
        // Let the window follow the slide all the way to nothing: the default minimum is 32px, which
        // would hold the drawer open a third of an inch and then pop it, instead of closing smoothly.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0.0f, 0.0f));
        ImGui::Begin("##drawer", nullptr, kDrawerFlags);

        // Drag the top edge to resize. The grip is claimed before anything else is submitted so it
        // always wins the hit test against the panel body underneath it.
        const f32 gripH = 5.0f * dpi_;
        ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
        ImGui::InvisibleButton("##drawergrip", ImVec2(std::fmax(wsize.x, 1.0f), gripH));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive() && wsize.y > statusH + 1.0f) {
            drawerFrac_ -= ImGui::GetIO().MouseDelta.y / (wsize.y - statusH);
            drawerFrac_ = std::fmin(0.88f, std::fmax(0.14f, drawerFrac_));
        }
        // The grip was placed at x=0 to span the full width; put the cursor back inside the padding so
        // the header and body are not flush against the window edge.
        ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x, gripH + ImGui::GetStyle().WindowPadding.y));
        ImGui::GetWindowDrawList()->AddLine(ImVec2(wpos.x, ImGui::GetWindowPos().y),
                                            ImVec2(wpos.x + wsize.x, ImGui::GetWindowPos().y),
                                            ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);

        // Header: which drawer this is, and the ways out of it.
        if (fontMedium_) ImGui::PushFont(fontMedium_, 0.0f);
        ImGui::TextUnformatted(drawerShown_ == Drawer::Log ? "Output Log" : "Content Browser");
        if (fontMedium_) ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled(drawerShown_ == Drawer::Content ? "(Ctrl+Space or Esc to dismiss)" : "(Esc to dismiss)");
        ImGui::SameLine(std::fmax(ImGui::GetCursorPosX(), wsize.x - 34.0f * dpi_));
        if (ImGui::Button("X")) drawer_ = Drawer::None;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close the drawer");
        ImGui::Separator();

        // Only draw the body once there is room for it: mid-slide the window is a few pixels tall, and
        // a Content Browser laid out into that would thrash its column arithmetic for one frame.
        if (ImGui::GetContentRegionAvail().y > ImGui::GetFrameHeight()) {
            if (drawerShown_ == Drawer::Log) drawOutputLog();
            else                             drawContentBrowser();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    // Open a drawer, or close it if it is already the one showing -- the toggle behind both the
    // status-bar buttons and Ctrl+Space.
    void toggleDrawer(Drawer d) {
        drawer_ = (drawer_ == d) ? Drawer::None : d;
        if (drawer_ != Drawer::None) drawerRaise_ = true;
    }

    // Editor Preferences — how the EDITOR behaves, as opposed to Project Settings, which is what the
    // GAME is. The split is the same one Unreal draws, and it is the one that decides where a setting
    // belongs: anything here is this machine's taste and would be wrong to write into a project a
    // colleague also opens.
    void buildEditorPrefs() {
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
            // Which IDE a double-click on source opens. "Automatic" is first and is the default,
            // because it is also the entry that works on a machine with no IDE installed.
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
        if (ImGui::CollapsingHeader("Viewport", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Show grid", &showGrid_);
            ImGui::Checkbox("Wireframe", &wireframe_);
            ImGui::SliderFloat("Fly speed", &flySpeed_, 1.0f, 200.0f, "%.0f");
            ImGui::SliderFloat("Look sensitivity", &lookSpeed_, 0.001f, 0.02f, "%.4f");
        }
        ImGui::Separator();
        ImGui::TextDisabled("Preferences apply immediately and last for this session.");
        ImGui::TextDisabled("They are not written to disk yet - there is no editor config file.");
        ImGui::End();
    }

    // Project Settings — a floating, categorised window like Unreal's, opened from
    // Edit > Project Settings. Rendering quality is project-wide, so it lives here rather than
    // in the per-actor Details panel.
    void buildProjectSettings() {
        if (focusVoxi_ > 0) { showProjectSettings_ = true; --focusVoxi_; } // --project-settings (screenshot aid)
        if (!showProjectSettings_) return;

        const ImGuiViewport* mv = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(880.0f*dpi_, 560.0f*dpi_), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(mv->GetCenter().x, mv->GetCenter().y), ImGuiCond_FirstUseEver, ImVec2(0.5f,0.5f));
        if (!ImGui::Begin("Project Settings", &showProjectSettings_, ImGuiWindowFlags_NoDocking)) { ImGui::End(); return; }

        // Category sidebar (left) + settings page (right).
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
                ImGui::TextDisabled("These are read-only: the editor loads .ocproject but does not");
                ImGui::TextDisabled("write it back yet.");
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
    // Voxi render settings page. Every feature reports its real status, so a toggle is never shown
    // as available when the renderer or the GPU cannot actually do it.
    void buildRenderingSettings() {
        using namespace aver::voxi;
        Renderer& vx = Renderer::get();
        ImGui::TextUnformatted("Rendering");
        ImGui::SameLine(); ImGui::TextDisabled("(Voxi render module)");
        ImGui::Separator();

        Settings s = vx.settings();
        bool changed = false;
        // ImGui draws labels to the RIGHT of a widget, so leave them room.
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);

        // --- anti-aliasing (implemented: really rebuilds the targets and PSOs) ---
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

        // --- quality-ladder features ---
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

        // GI tunables stay visible (greyed) so the shape of the feature is discoverable.
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

        // Geometry submission path: mesh shaders vs the classic vertex/geometry pipeline.
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

        if (changed) vx.setSettings(s);
        // No ImGui::End() here: this renders as a page inside the Project Settings child region.
    }
#endif

    // Unreal puts the transform tools and snapping in the VIEWPORT's own overlay bar, not the
    // window toolbar: left group = view options, right group = tools + snapping + camera speed.
    void buildViewportOverlay() {
        if (vpW_ < 80.0f || vpH_ < 60.0f) return;
        const f32 pad = 8.0f*dpi_;
        const ImGuiWindowFlags f = ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|
                                   ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_AlwaysAutoResize|
                                   ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoSavedSettings|
                                   ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoNavFocus;

        // --- left group: view type / view mode / show flags ---
        ImGui::SetNextWindowPos(ImVec2(vpX_+pad, vpY_+pad), ImGuiCond_Always, ImVec2(0,0));
        ImGui::SetNextWindowBgAlpha(0.62f);
        ImGui::Begin("##vpbar_left", nullptr, f);
        if (dropButton("Perspective")) ImGui::OpenPopup("viewType");
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
        if (ImGui::BeginPopup("showFlags")) {
            ImGui::Checkbox("Grid", &showGrid_);
            bool t=true; ImGui::Checkbox("Static Meshes", &t);
            ImGui::Checkbox("Atmosphere", &t);
            ImGui::EndPopup();
        }
        ImGui::End();

        // --- right group: transform tools + snapping + camera speed ---
        const f32 icon = 26.0f*dpi_, caretW = 14.0f*dpi_, tiny = 2.0f*dpi_, gap = 6.0f*dpi_;
        ImGui::SetNextWindowPos(ImVec2(vpX_+vpW_-pad, vpY_+pad), ImGuiCond_Always, ImVec2(1,0)); // right-aligned
        ImGui::SetNextWindowBgAlpha(0.62f);
        ImGui::Begin("##vpbar_right", nullptr, f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto toolBtn = [&](const char* id, int kind, bool active)->bool {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton(id, ImVec2(icon, icon));
            const bool hov = ImGui::IsItemHovered(), clk = ImGui::IsItemClicked();
            const ImU32 bg = active ? IM_COL32(232,110,35,235) : (hov ? IM_COL32(74,76,82,255) : IM_COL32(48,49,54,220));
            dl->AddRectFilled(p, ImVec2(p.x+icon,p.y+icon), bg, 4.0f);
            drawToolGlyph(dl, p, icon, kind, IM_COL32(236,237,240,255));
            return clk;
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
        ImGui::SameLine(0, gap);
        if (toolBtn("##tMove", 1, tool_==Tool::Move)) tool_=Tool::Move;
        ImGui::SameLine(0, tiny); if (caretBtn("##cMove", snapMove_)) ImGui::OpenPopup("snapMove");
        ImGui::SameLine(0, gap);
        if (toolBtn("##tRot", 2, tool_==Tool::Rotate)) tool_=Tool::Rotate;
        ImGui::SameLine(0, tiny); if (caretBtn("##cRot", snapRot_)) ImGui::OpenPopup("snapRot");
        ImGui::SameLine(0, gap);
        if (toolBtn("##tScl", 3, tool_==Tool::Scale)) tool_=Tool::Scale;
        ImGui::SameLine(0, tiny); if (caretBtn("##cScl", snapScale_)) ImGui::OpenPopup("snapScale");
        ImGui::SameLine(0, gap*2);
        if (ImGui::Button(worldSpace_ ? "World" : "Local")) worldSpace_ = !worldSpace_; // coord space, like UE's globe/cube
        ImGui::SameLine(0, gap);
        char camLbl[32]; std::snprintf(camLbl, sizeof camLbl, "Cam %.0f", flySpeed_);
        if (dropButton(camLbl)) ImGui::OpenPopup("camSpeed");
        if (ImGui::BeginPopup("camSpeed")) { ImGui::SliderFloat("Speed", &flySpeed_, 1.0f, 200.0f, "%.0f"); ImGui::EndPopup(); }

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

        // Bottom-left hint, anchored to the viewport like Unreal's transform readout.
        ImGui::SetNextWindowPos(ImVec2(vpX_+pad, vpY_+vpH_-pad), ImGuiCond_Always, ImVec2(0,1));
        ImGui::SetNextWindowBgAlpha(0.35f);
        ImGui::Begin("##vphint", nullptr, f | ImGuiWindowFlags_NoInputs);
        ImGui::Text("%s  |  RMB fly (WASD/QE)  wheel speed  MMB pan  F focus  |  1-4 tools", kToolNames[(int)tool_]);
        ImGui::End();
    }
#endif

    void captureCheck(Engine& e) {
        const u64 f = e.time().frame;
        const u64 sf = maxFrames_>8?maxFrames_-3:4;
        // Sample the centre of the 3D viewport, not the window: with panels docked the window
        // centre can land on UI, which would silently stop verifying that the scene rasterises.
        // `--probe X Y` overrides it with absolute backbuffer pixels. Needed because the centre
        // lands on the cube's UNLIT left face, where ndl is ~0 and the sun term drops out entirely
        // -- so any shadow/ray-tracing A/B must probe a sunlit or cast-shadow pixel instead.
        const u32 px_ = probeX_ ? probeX_ : (u32)(vpX_ + vpW_*0.5f);
        const u32 py_ = probeY_ ? probeY_ : (u32)(vpY_ + vpH_*0.5f);
        if (f==sf) {
            e.device()->requestCapture(px_, py_);
            // Latch the rect the request was made against. The rect is written by buildUI and can
            // change under a resize between the request and the read, and it is the rect AT REQUEST
            // TIME that decides what the captured pixel actually shows.
            capX_=px_; capY_=py_; capVpX_=vpX_; capVpY_=vpY_; capVpW_=vpW_; capVpH_=vpH_;
        }
        if (f>sf && !capDone_){
            // A pixel outside the 3D viewport samples editor chrome -- the dock clear colour reads
            // as raw(14,14,16) and looks exactly like a shading result to anything grepping for a
            // raw code. That has already been misread as a rendering regression once, so the probe
            // line states the rect it was taken against and tags the sample IN/OUTSIDE it. The
            // value is still printed: a suppressed number is a different way to be misread.
            const bool insideReq = (f32)capX_ >= capVpX_ && (f32)capX_ < capVpX_+capVpW_ &&
                                   (f32)capY_ >= capVpY_ && (f32)capY_ < capVpY_+capVpH_;
            // Compared against the CURRENT rect too, because a resize between request and read
            // means the latched rect no longer describes what was drawn.
            const bool rectStable = capVpX_==vpX_ && capVpY_==vpY_ && capVpW_==vpW_ && capVpH_==vpH_;
            const char* tag = !insideReq ? "OUTSIDE-VIEWPORT"
                            : !rectStable ? "VIEWPORT-MOVED"
                                          : "in-viewport";
            // The raw 8-bit codes as well as the rounded floats: at two decimal places a whole code
            // of movement can hide inside one printed digit, which is exactly how a one-code-wide
            // wobble in the GI path went unnoticed while a three-code one did not.
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
    int sel_ = 1;
    Tool tool_ = Tool::Select;
    // Free-fly editor camera (Unreal-style): position + yaw/pitch, no auto-orbit.
    Vec3 camPos_{7.0f, 7.0f, 4.5f};
    f32 yaw_ = 0.0f, pitch_ = 0.0f, flySpeed_ = 12.0f, lookSpeed_ = 0.005f;
    bool flying_ = false;
    // The selected object's transform and mesh, latched during the scene pass so the outline can be
    // drawn after every surface is down rather than in the middle of the loop.
    Mat4 selectionOutline_{}; rhi::MeshHandle selectionMesh_ = 0; bool hasSelection_ = false;
    // sun
    f32 sunAz_=-0.55f, sunAlt_=-0.45f, sunUp_=0.55f, sunColor_[3]={1.0f,0.96f,0.9f}, sunAmbient_=0.28f;
    // sky + atmosphere
    f32 skyZenith_[3]={0.19f,0.42f,0.78f}, skyHorizon_[3]={0.72f,0.80f,0.90f};
    f32 fogColor_[3]={0.70f,0.78f,0.88f}, fogDensity_=0.014f;
    // The camera's post chain, at its identity defaults. See rhi::PostSettings for why they are the
    // identity and not something prettier.
    rhi::PostSettings post_{};
    bool capDone_=false;
    // The pixel actually requested and the viewport rect it was requested against, latched at the
    // request frame so the report a few frames later describes the state that produced the value.
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
    // 3D viewport rect = the dockspace's central node, in backbuffer pixels. Latched by buildUI
    // and consumed next frame by the camera aspect, the scene scissor, picking and the gizmo.
    f32 vpX_=0, vpY_=0, vpW_=1600, vpH_=900;
    bool dockBuilt_=false;   // one-shot DockBuilder layout (nothing is persisted to an ini)
    bool showProjectSettings_=false; // Edit > Project Settings window
    bool showEditorPrefs_=false;     // Edit > Editor Preferences window
    int  settingsPage_=1;            // 0 = Description, 1 = Rendering
    int  focusVoxi_=0;               // --project-settings: frames left to force the window open
    int  msaaOverride_=0;            // --msaa N: apply a sample count at startup
    int  giOverride_=0;              // --gi: GI quality to apply at startup
    int  rtOverride_=0;              // --rt: ray tracing quality at startup
    bool msOverride_=false;          // --ms: force the mesh shader geometry path
    u32  probeX_=0, probeY_=0;       // --probe X Y: absolute capture pixel (0 = viewport centre)
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
    // Project browser + the project it produced. `browserActive_` is false for every automated
    // run, so the oracle never sees the start screen.
    editor::ProjectBrowser browser_;
    fmt::ProjectDesc project_;
    std::string projectPath_;        // <path>.ocproject given on the command line
    bool browserActive_=false;
    // The start screen's mark. Zero when the screen was never armed, or when logo.png was missing
    // or undecodable -- in which case the browser draws its fallback badge instead.
    rhi::TextureHandle logoTexture_=0;
    rhi::TextureHandle fileIconsTexture_=0;     // the Content Browser file-type sprite sheet (4 tiles)
    u64 fileIconsUiId_=0;
    f32 fileIconAspect_=0.74f;                  // measured from the sheet; the literal is only the fallback
    rhi::TextureHandle folderIconsTexture_=0;   // the folder sheet (2 tiles: plain, module)
    u64 folderIconsUiId_=0;
    f32 folderIconAspect_=1.24f;
    // path -> (mtime, tile), so a .cs is classified once but RE-classified when it is edited: a file
    // that gains an [AverClass] mid-session must stop showing the plain-class icon.
    std::unordered_map<std::string, std::pair<std::filesystem::file_time_type, int>> fileIconCache_;
    // Throttled directory listings for the Content Browser, so a folder is not re-walked every frame.
    std::unordered_map<std::string, DirListing> dirCache_;
    int frameNo_ = 0;                                      // bumped once per UI frame; the cache freshness clock
    rhi::TextureHandle compileIconTexture_=0;   // the Compile C# status sprite sheet (3 tiles)
    u64 compileIconUiId_=0;
    u64 logoUiId_=0;
    f32 logoAspect_=1.0f;
    // The Tools menu owns its own dropdown, its modals and its scaffolding (ToolsMenu.cpp).
    editor::ToolsMenu tools_;
    bool worldSpace_=true;   // gizmo coordinate space toggle (display only for now)
    // Voxi GI volume placement: a cube around the default scene (floor is +/-40, cube at origin).
    bool giDebugView_=false; Vec3 giCenter_{0,0,8}; f32 giExtent_=44.0f;
#if AVER_MODULE_VOXI
    // Voxi's GPU side. Inert for now: it creates its resources and leaves them idle.
    voxi::VoxiRenderer voxiRenderer_;
    bool voxiAttached_=false;
#endif
#if AVER_MODULE_SCRIPTING
    // The in-process CLR. Owned by the app, like the Voxi feature, because the app is what
    // configures it -- the composition root has no business knowing scripting exists.
    scripting::ScriptHost scripts_;
#endif
    rhi::MeshHandle cubeMesh_=0; u32 cubeTris_=0; int spawnCount_=0;
    // Named surfaces a spawned actor can ask for by name, keyed by the token aver_scene_material
    // interns. A stand-in for authored materials, so a level can read as a place rather than as one
    // undifferentiated grey mass. See the scene-render pass for why it exists.
    struct SurfaceLook { f32 col[3]; f32 metallic; f32 roughness; };
    std::unordered_map<i32, SurfaceLook> surfaceLooks_;
#if AVER_MODULE_PBR
    // Where the texture resolver gets its factory. Cached at init rather than reached through the
    // Engine, because the resolver is a static callback the material system invokes from inside its
    // own dirty drain -- there is no Engine& in scope there, and there must not be.
    rhi::IResourceFactory* textureFactory_ = nullptr;
    // fnv1a64(content-relative path) -> absolute path, for `{guid:...}` texture references.
    std::unordered_map<u64, std::string> contentIndex_;
    // Surface NAME -> the material loaded from its .ocmat. Holds 0 for a name with no asset, which
    // is a cached negative rather than a miss to retry.
    std::unordered_map<std::string, pbr::MaterialHandle> materialAssets_;
    // The same answer keyed by the token the scene interns, which is what a CMeshRenderer carries.
    // Filled at level load: the render pass has an i32 and no way back to the string, and adding a
    // reverse lookup to the scene ABI to serve one editor pass would be the wrong place to put it.
    std::unordered_map<i32, pbr::MaterialHandle> surfaceMaterials_;
#endif
    // ---- levels (.ocworld) ---------------------------------------------------------------------
#if AVER_MODULE_SCENE
    // Load the project's start map into the world, as ordinary scene entities.
    //
    // These are NOT actors: they carry a transform, a mesh and a name, and nothing else. That is the
    // point of a level being data -- it is visible and selectable in the editor without a play session
    // existing, and gameplay does not have to run for the world to be there.
    //
    // Gate-neutral: the gates load no project, so there is no start map and this never runs.
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
                // Resolve the surface name to a real material HERE, where the name is still in hand.
                // The render pass only ever sees the interned token, and materialForSurface caches
                // both hits and misses, so a level of a thousand placements naming six surfaces does
                // six file lookups in total.
                if (mr->material) {
                    const pbr::MaterialHandle h = materialForSurface(p.material);
                    if (h) surfaceMaterials_[mr->material] = h;
                }
#endif
            }
            levelEntities_.push_back(e);

#if AVER_MODULE_PHYSICS
            // A level's collision comes from the level, not from a script that happens to run later.
            if (p.collide && aver_phys_ready())
                levelBodies_.push_back(aver_phys_add_static_box(
                    static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z),
                    static_cast<f32>(p.sx), static_cast<f32>(p.sy), static_cast<f32>(p.sz)));
#endif
        }

        // The level owns its own environment when it says so, which is how a centimetre-scale world
        // stops inheriting fog tuned for the editor's placeholder scene.
        if (w.hasFog) {
            levelFog_ = static_cast<f32>(w.fogDensity);
            fogColor_[0] = static_cast<f32>(w.fogColor[0]);
            fogColor_[1] = static_cast<f32>(w.fogColor[1]);
            fogColor_[2] = static_cast<f32>(w.fogColor[2]);
            hasLevelFog_ = true;
        }
        levelPath_ = path;
        levelName_ = w.name;

        // Frame the camera on what was just loaded.
        //
        // Without this the level loads correctly and is invisible: the editor's default camera sits
        // 7 units from the origin, which was framed for the placeholder scene at roughly a unit per
        // metre, and a level authored in CENTIMETRES is a hundred times larger around it. Everything
        // is drawn and the viewer is standing inside the floor slab, which reads as "the map did not
        // load" -- the one conclusion that is wrong.
        if (!w.placements.empty()) frameCameraOn(w);

        AVER_INFO("[Level] '{}' loaded from {} ({} placement(s))", w.name, path, w.placements.size());
    }

    // Put the editor camera where the whole level is visible: back off along a diagonal by enough
    // that the bounding sphere fits the vertical field of view, and look at its centre.
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
        // 45 degrees up and behind, at 1.6 radii: far enough that the whole thing fits with margin,
        // close enough that it fills the frame rather than sitting in the middle of an empty sky.
        const f32 dist = radius * 1.6f;
        camPos_ = Vec3{centre.x - dist * 0.65f, centre.y - dist * 0.65f, centre.z + dist * 0.55f};
        const Vec3 look = (centre - camPos_).getSafeNormal();
        yaw_   = std::atan2(look.y, look.x);
        pitch_ = std::asin(std::fmax(-1.0f, std::fmin(1.0f, look.z)));
        // The fly speed is per-frame centimetres here, so a level this size needs a bigger step than
        // the placeholder scene's 12 or crossing the room takes half a minute.
        flySpeed_ = std::fmax(flySpeed_, radius * 0.02f);

#if AVER_MODULE_VOXI
        // ...and so does the GI volume, for the same reason and with the same failure mode as the
        // grid and the fog. Its default (centre 0,0,8 extent 44) is authored for the placeholder
        // scene at roughly a unit per metre; a centimetre-scale level got a FORTY-FOUR CENTIMETRE
        // box, so cone-traced GI covered a patch of floor smaller than the player and every other
        // surface fell back to sky ambient. Fitted to the level here, where the bounds are already
        // in hand for the camera.
        //
        // Not conditioned on anything: this runs only when a level loads, and the gates load none.
        giCenter_ = centre;
        giExtent_ = radius;
#endif
    }

    // The project's STARTMAP, resolved against its content directory. Missing is not an error: a new
    // project has no level yet, and saying so once is more useful than a warning every launch.
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

    void unloadLevel() {
        scene::World& world = scene::World::instance();
        for (const scene::Entity e : levelEntities_) if (world.valid(e)) world.destroy(e);
        levelEntities_.clear();
#if AVER_MODULE_PHYSICS
        for (const int32_t b : levelBodies_) aver_phys_remove_body(b);
        levelBodies_.clear();
#endif
        hasLevelFog_ = false;
        levelPath_.clear();
        // Materials are deliberately NOT released here. They are PROJECT-scoped, not level-scoped:
        // a script that spawns an actor mid-play names the same surfaces the level does, and
        // dropping them on a level change would leave everything spawned afterwards on the fallback.
        // releaseProjectMaterials() owns their lifetime.
    }

    // Write the level's entities back out. Only the entities THIS level owns are written: a play
    // session's spawned actors share the same world, and saving them would bake a running game's
    // transient state into the level file.
    bool saveLevel(const std::string& path) {
        scene::World& world = scene::World::instance();
        fmt::OcWorldData w;
        w.name = levelName_.empty() ? std::string("untitled") : levelName_;
        w.hasFog = hasLevelFog_;
        w.fogDensity = levelFog_;
        w.fogColor[0] = fogColor_[0]; w.fogColor[1] = fogColor_[1]; w.fogColor[2] = fogColor_[2];

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
    bool hasLevelFog_ = false;
    f32  levelFog_ = 0.0002f;
#if AVER_MODULE_PHYSICS
    std::vector<int32_t> levelBodies_;
#endif
#endif // AVER_MODULE_SCENE

    // ---- mouse capture -------------------------------------------------------------------------
    // While a game is playing the mouse belongs to the GAME: the cursor is hidden, confined to the
    // window, and re-centred every frame so mouse-look has no edge to run into. Shift+F1 hands it
    // back, which is the shortcut Unreal uses for the same thing and therefore the one people try.
    //
    // The delta is measured against the point we last warped the cursor to, NOT ImGui's MouseDelta:
    // warping makes ImGui see a jump every frame, so its delta is meaningless while captured.
    bool mouseCaptured_ = false;
    i32  captureAnchorX_ = 0, captureAnchorY_ = 0;
    f32  captureDx_ = 0.0f, captureDy_ = 0.0f;
    Window* window_ = nullptr;   // borrowed from the engine in onInit, for the HWND
    bool releasedByUser_ = false;   // Shift+F1 during a session; cleared when the session ends

    void setMouseCaptured(bool on) {
#if defined(_WIN32)
        if (on == mouseCaptured_) return;
        mouseCaptured_ = on;
        if (on) {
            // ShowCursor is a COUNTER, not a flag, so it must be paired exactly once with its undo --
            // calling it twice leaves the cursor hidden after release, with no way for the user to
            // get it back short of restarting the editor.
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
    // Park the cursor at the centre of the window and remember where that was.
    void warpToAnchor() {
        HWND hwnd = window_ ? static_cast<HWND>(window_->nativeHandle()) : nullptr;
        if (!hwnd) return;
        RECT rc{};
        if (!GetClientRect(hwnd, &rc)) return;
        POINT c{ (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
        ClientToScreen(hwnd, &c);
        captureAnchorX_ = c.x; captureAnchorY_ = c.y;
        SetCursorPos(c.x, c.y);
        // Confine to the window as well: without this a fast flick can leave the window and land a
        // click on whatever is behind it.
        RECT screen{};
        POINT tl{ rc.left, rc.top }, br{ rc.right, rc.bottom };
        ClientToScreen(hwnd, &tl); ClientToScreen(hwnd, &br);
        screen.left = tl.x; screen.top = tl.y; screen.right = br.x; screen.bottom = br.y;
        ClipCursor(&screen);
    }

    // One frame of captured mouse movement, then re-centre for the next.
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
    // The world's floor. Centimetres, per the physics ABI: a 100m square, 10cm thick, centred so its
    // TOP face sits exactly on z=0 -- so "the ground is at zero" is true for gameplay that assumes it.
    static constexpr f32 kGroundHalfExtentCm = 5000.0f;
    static constexpr f32 kGroundHalfThickCm  = 5.0f;
    int32_t groundBody_ = 0;
#endif
    // Output Log capture. Written by logSink from any thread under logMutex_; read by the panel on the UI
    // thread under the same lock. Bounded so a long session cannot grow it without limit.
    static constexpr size_t kMaxLogLines = 4000;
    std::mutex          logMutex_;
    std::deque<LogLine> logLines_;
    bool                logAutoScroll_ = true;
    int                 logLevelFilter_ = 0;      // 0 = all, 1 = Info+, 2 = Warn+
    // Content Browser: the folder whose files are listed, and the Import modal's source-path field.
    std::string         cbSelectedDir_;           // empty -> the content root
    char                importPath_[512] = {};
    bool                cbGallery_ = true;        // tiles vs list; tiles is the default, as in UE
    f32                 cbTileSize_ = 88.0f;      // gallery tile edge, in dp, driven by the zoom slider
    std::string         cbSelectedFile_;          // the highlighted entry in the file view
    char                cbFilter_[128] = {};      // the search box: filters the open folder by name
    // Where the "Engine" root mounts from -- the engine's C# classes. Empty in a shipped build, which
    // simply means the root is not offered.
    std::string         cbEngineRoot_;
    // Back/Forward history, as a file manager has it: every navigation appends, and going back then
    // somewhere new forks rather than interleaving.
    std::vector<std::string> cbHistory_;
    int                 cbHistoryPos_ = -1;
    // The right-click target. Held separately from the selection because a context menu acts on what
    // was right-clicked, which is not necessarily what was selected.
    std::string         cbContextPath_;
    bool                cbContextIsDir_ = false;
    // Deferred popup requests. Set from inside the file view's child window, acted on at the panel
    // level: an ImGui popup id is scoped to the window that opens it, so opening from the child and
    // drawing from the parent would never match.
    bool                cbWantRename_ = false, cbWantDelete_ = false, cbWantNewFolder_ = false;
    bool                cbWantDuplicate_ = false, cbWantImport_ = false;
    char                cbRenameBuf_[256] = {};
    char                cbNewFolderBuf_[128] = {};
    std::string         cbStatus_;                // last operation's outcome, shown in the footer
    // Which detected IDE a double-click opens source in. -1 = whatever IdeIntegration prefers, which
    // is the right default because it is also the one that exists on a machine with nothing installed.
    int                 cbIdeChoice_ = -1;
    // The bottom drawers. Both panels start CLOSED -- "all the way down" -- and slide up on demand,
    // from Ctrl+Space or the status-bar buttons, over the viewport rather than stealing a dock node
    // from it. drawerAnim_ is the eased 0..1 slide so the panel does not snap into place, and
    // drawerShown_ is what to KEEP DRAWING while it retracts, after drawer_ has already gone to None.
    Drawer              drawer_ = Drawer::None;
    Drawer              drawerShown_ = Drawer::Content;
    f32                 drawerAnim_ = 0.0f;
    // Drawer height as a fraction of the work area. 0.48 rather than something smaller so the default
    // gallery shows a full row INCLUDING its two label lines -- a first row whose captions are cut off
    // reads as broken rather than as scrollable.
    f32                 drawerFrac_ = 0.48f;
    f32                 drawerRate_ = 14.0f;      // slide easing rate; higher is snappier
    bool                cbDoubleClickEnter_ = true;   // double-click a folder to enter it (vs single)
    bool                drawerRaise_ = false;     // focus it on the frame it opens, so it is on top
    std::string         drawerStartSub_;          // --drawer content:<sub>, applied once at first draw
#if AVER_MODULE_SCENE
    // ObjectId -> built-in primitive mesh, for the scene-render pass: a spawned actor names its mesh by
    // the fnv1a64 of a path, and this resolves it to a handle. Small and fixed for now (just the sphere).
    std::unordered_map<u64, rhi::MeshHandle> sceneMeshes_;
    int lastSceneDrawn_=-1;           // last scene-entity draw count, so the log line fires only on change
#endif
    Mat4 invVP_, viewProj_; Vec3 eye_{0,0,0};
};

// True for "<something>.ocproject", so a positional argument can be either a project manifest or
// the .ocbeam the sandbox has always accepted, without a new flag for it.
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

Application* createApplication(int argc, char** argv) {
    u64 frames=0; bool headless=false, focusVoxi=false, focusScript=false, focusTools=false, focusCompile=false, startScreen=false; int drawerOpen=0; std::string drawerSub; std::string beam, shot, project, scriptsDir, spawnTest; bool playTest=false; Tool tool=Tool::Select; int msaa=0; int gi=0; int rt=0; bool giDbg=false, ms=false; u32 probeX=0, probeY=0; int reloadAt=0; bool warp=false, debugLayer=false; const char* forceCaps=nullptr; f32 bloom=0.0f, exposure=1.0f; bool autoExposure=false;
    for (int i=1;i<argc;++i){
        if (!std::strcmp(argv[i],"--headless")) headless=true;
        else if (!std::strcmp(argv[i],"--project-settings")) focusVoxi=true;
        else if (!std::strcmp(argv[i],"--new-script")) focusScript=true;
        // Holds the Tools dropdown open so it can be photographed. Opt-in, like the two above:
        // it changes only what hangs BELOW the menu bar, never the bar's height, but no oracle
        // gate passes it and none can reach it by accident.
        else if (!std::strcmp(argv[i],"--tools-menu")) focusTools=true;
        else if (!std::strcmp(argv[i],"--compile-scripts")) focusCompile=true;
        // --reload-scripts [N] fires Tools > Reload Scripts once, N frames in (default 20). Same
        // family as the three above, and the only way to prove a reload without a mouse: the point
        // of the feature is that it happens to an editor that is ALREADY running something, so the
        // delay is the test rather than a convenience — it leaves room to edit the .cs on disk
        // between the initial load and the swap.
        else if (!std::strcmp(argv[i],"--reload-scripts")) {
            reloadAt = (i+1 < argc && argv[i+1][0] != '-') ? std::atoi(argv[++i]) : 20;
        }
        else if (!std::strcmp(argv[i],"--start-screen")) startScreen=true;
        // Screenshot aid in the --project-settings family: open a bottom drawer that a capture run
        // has no way to toggle interactively. `content:<sub>` starts inside a Content subfolder.
        else if (!std::strcmp(argv[i],"--drawer") && i+1<argc) {
            const char* v = argv[++i];
            drawerOpen = !std::strcmp(v,"log") ? 2 : 1;
            if (const char* colon = std::strchr(v, ':')) drawerSub = colon + 1;
        }
        else if (!std::strcmp(argv[i],"--msaa") && i+1<argc) msaa=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--gi")) gi=3;
        else if (!std::strcmp(argv[i],"--gi-debug")) { gi=3; giDbg=true; }
        else if (!std::strcmp(argv[i],"--rt")) rt=3;
        else if (!std::strcmp(argv[i],"--ms")) ms=true;
        else if (!std::strcmp(argv[i],"--probe") && i+2<argc) { probeX=(u32)std::atoi(argv[++i]); probeY=(u32)std::atoi(argv[++i]); }
        // Fallback-path testing. `--force-caps` clamps what the device reports it can do and
        // `--warp` swaps the adapter for the software rasteriser. Both exist because the engine
        // has only ever run on one GPU, so every capability gate in it is reasoned rather than
        // measured; see docs/STATUS.md §4g. Neither can raise a capability above the hardware's.
        else if (!std::strcmp(argv[i],"--force-caps") && i+1<argc) forceCaps=argv[++i];
        else if (!std::strcmp(argv[i],"--warp")) warp=true;
        // The graphics debug layer, off unless asked for. It validates every API call, so it is a
        // per-call cost a normal run must not pay; `scripts/gates.ps1` passes it because the
        // per-gate corruption/error/warning counters are read out of it.
        else if (!std::strcmp(argv[i],"--debug-layer")) debugLayer=true;
        // Where the scripting host looks for user assemblies. Relative to the executable unless
        // absolute; the default (<exe>\Scripts) does not exist in a clean build, so no gate loads
        // anything. `--scripts SampleScripts` picks up the staged sample behaviour.
        else if (!std::strcmp(argv[i],"--scripts") && i+1<argc) scriptsDir=argv[++i];
        else if (!std::strcmp(argv[i],"--spawn-test") && i+1<argc) spawnTest=argv[++i];
        else if (!std::strcmp(argv[i],"--play-test")) playTest=true;
        else if (!std::strcmp(argv[i],"--frames") && i+1<argc) frames=std::strtoull(argv[++i],nullptr,10);
        else if (!std::strcmp(argv[i],"--screenshot") && i+1<argc) shot=argv[++i];
        // Camera post, for capture runs: the chain's non-default states have no other way in from a
        // headless run, and a feature nothing can screenshot is a feature nobody can check.
        else if (!std::strcmp(argv[i],"--bloom") && i+1<argc) bloom=static_cast<f32>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i],"--exposure") && i+1<argc) exposure=static_cast<f32>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i],"--auto-exposure")) autoExposure=true;
        else if (!std::strcmp(argv[i],"--tool") && i+1<argc) {
            const char* t=argv[++i];
            tool = !std::strcmp(t,"move")?Tool::Move : !std::strcmp(t,"rotate")?Tool::Rotate :
                   !std::strcmp(t,"scale")?Tool::Scale : Tool::Select;
        }
        else if (argv[i][0]!='-') { if (isOcproject(argv[i])) project=argv[i]; else beam=argv[i]; }
    }
    // Set before the engine creates a device: the clamp has to be in place by the time the
    // backend queries the hardware, which happens inside Engine::run.
    if (forceCaps && !rhi::setCapsOverride(forceCaps))
        AVER_ERROR("[Sandbox] --force-caps '{}' was rejected; running on the UNCLAMPED device", forceCaps);

    auto* app = new SandboxApp(frames, headless, beam, shot, tool);
    app->setPost(exposure, bloom, autoExposure);
    app->setUseWarp(warp);
    app->setDebugLayer(debugLayer);
    app->setProjectPath(project);
    // The start screen is for a human opening the editor with nothing to open. It must never
    // appear in automation: every gate in the verification harness passes --frames and reads a
    // probe pixel out of the viewport, which a full-screen chooser would cover. `--frames`
    // present, a project already named, or headless => straight to the editor.
    //
    // `--start-screen` forces it back on, and is a screenshot aid in the same family as
    // --project-settings and --new-script: the start screen is otherwise unreachable together with
    // --frames/--screenshot, so it could not be captured through the engine's own backbuffer path
    // at all. Opt-in, so no gate can reach it by accident.
    app->armBrowser(startScreen || (!headless && frames == 0 && project.empty()));
    app->setFocusVoxi(focusVoxi);
    app->setDrawerOpen(drawerOpen, drawerSub);
    app->setFocusScript(focusScript);
    app->setFocusTools(focusTools);
    app->setFocusCompile(focusCompile);
    app->setFocusReload(reloadAt);
    app->setMsaaOverride(msaa);
    app->setGiOverride(gi, giDbg);
    app->setRtOverride(rt);
    app->setMsOverride(ms);
    app->setProbe(probeX, probeY);
    app->setScriptsDir(scriptsDir);
    app->setSpawnTest(spawnTest);
    if (playTest) app->setPlayTest();
    return app;
}

} // namespace aver
