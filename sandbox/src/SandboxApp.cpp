#include "aver/runtime/EntryPoint.hpp"
#include "aver/platform/Window.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/platform/Image.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/core/Version.hpp"
#include "aver/formats/OcBeam.hpp"
#include "aver/formats/OcProject.hpp"

#include "ProjectBrowser.hpp"
#include "ProjectScaffold.hpp"

#if AVER_MODULE_VOXI
#include "aver/voxi/Voxi.hpp"          // optional render-feature module (AA / GI / RT / PT settings)
#include "aver/voxi/VoxiRenderer.hpp"  // ...and its GPU side, registered as an rhi::IRenderFeature
#endif

#if AVER_WITH_IMGUI
#include "imgui.h"
#include "imgui_internal.h" // DockBuilder* (docking layout is built in code: IniFilename is null)
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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
static Quat quatFromEulerDeg(const Vec3& e) {
    return (Quat::fromAxisAngle({0,0,1}, radians(e.z)) * Quat::fromAxisAngle({0,1,0}, radians(e.y)) *
            Quat::fromAxisAngle({1,0,0}, radians(e.x))).normalized();
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

#if AVER_WITH_IMGUI
// Fixed editor chrome (toolbar / status bar / dock host): no decoration, never steals focus.
static constexpr ImGuiWindowFlags kChromeFlags =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
#endif

struct MeshObj {
    std::string name;
    rhi::MeshHandle mesh = 0;
    u32 tris = 0;
    Vec3 pos{0,0,0}, rotDeg{0,0,0}, scale{1,1,1};
    f32 color[4] = {0.8f,0.4f,0.25f,1};
    f32 metallic = 0.0f, roughness = 0.5f;
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

class SandboxApp final : public Application {
public:
    SandboxApp(u64 maxFrames, bool headless, std::string beamPath, std::string shot, Tool initialTool)
        : maxFrames_(maxFrames), headless_(headless), beamPath_(std::move(beamPath)), shot_(std::move(shot)), initialTool_(initialTool) {}

    BootConfig config() const override {
        BootConfig c; c.windowTitle="Aver Engine \xE2\x80\x94 Editor"; c.windowWidth=1600; c.windowHeight=900;
        c.maxFrames=maxFrames_; c.headless=headless_; return c;
    }

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
#endif

    void onInit(Engine& e) override {
        AVER_INFO("[Sandbox] backend={} adapter='{}'", rhi::backendName(e.device()->backend()), e.device()->adapterName());

        // Always read the recent list, even when the start screen will not be shown: opening a
        // project records it, and recording into a list that was never loaded would truncate the
        // user's history to the one project the command line named.
        browser_.init();

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
        }
#endif
        // --- default "blank .ocmap": ground floor + cube + sun + sky + atmosphere ---
        std::vector<rhi::MeshVertex> gv, gi_v; std::vector<u32> gi, ci;
        appendGround(gv, gi, 40.0f);
        rhi::MeshHandle ground = e.device()->createMesh(gv.data(), (u32)gv.size(), gi.data(), (u32)gi.size());
        appendBox(gi_v, ci, 0,0,0, 1.0f);
        rhi::MeshHandle cube = e.device()->createMesh(gi_v.data(), (u32)gi_v.size(), ci.data(), (u32)ci.size());

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
            }
        }
#endif
        tool_ = initialTool_;
        sel_ = 1; // the Cube
        camPos_ = Vec3{7.0f, 7.0f, 4.5f};
        const Vec3 d = (Vec3{0,0,1} - camPos_).getSafeNormal();
        yaw_ = std::atan2(d.y, d.x);
        pitch_ = std::asin(d.z);
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
            if (ImGui::IsMouseClicked(1) && !overUI) flying_ = true;
            if (!io.MouseDown[1]) flying_ = false;

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
        const Mat4 proj = Mat4::perspectiveLH(radians(60.0f), aspect, 0.05f, 5000.0f);
        const Mat4 viewProj = view * proj;
        const Mat4 invVP = viewProj.inverse();
        e.device()->setCamera(&viewProj.m[0][0], &invVP.m[0][0], &camPos_.x);
        invVP_ = invVP; viewProj_ = viewProj; eye_ = camPos_;

        const Vec3 ld = Vec3{sunAz_, sunAlt_, sunUp_}.getSafeNormal();
        e.device()->setLight(&ld.x, sunColor_, sunAmbient_);
        e.device()->setSky(true, skyZenith_, skyHorizon_, fogColor_, fogDensity_);
        // Outside the viewport rect is editor chrome, not sky — clear to the dark panel colour.
        e.device()->setClearColor(0.055f, 0.055f, 0.062f, 1);
    }

    void onRender(Engine& e) override {
        handleManip(e);
        e.device()->setWireframe(wireframe_);
        for (int i=0;i<(int)objects_.size();++i) {
            MeshObj& o = objects_[i];
            if (!o.visible) continue;
            Transform tr; tr.position=o.pos; tr.rotation=quatFromEulerDeg(o.rotDeg); tr.scale=o.scale;
            Mat4 w = tr.toMatrix();
            f32 col[4]={o.color[0],o.color[1],o.color[2],1};
            if (i==sel_) for (int k=0;k<3;++k) col[k]=std::fmin(1.0f,col[k]*1.3f+0.10f);
            e.device()->drawMesh(o.mesh, &w.m[0][0], col, o.metallic, o.roughness);
        }
        e.device()->setWireframe(false); // lines are always solid
        if (showGrid_) { Mat4 id = Mat4::identity(); e.device()->drawLines(gridMesh_, &id.m[0][0]); }
        drawGizmo(e);
        buildUI(e);
        captureCheck(e);
    }

    void onShutdown(Engine& e) override {
#if AVER_WITH_IMGUI
        // The UI descriptor the mark holds is released with the texture, and that pool has no fence
        // of its own -- so the GPU has to be past every frame that drew it first.
        if (logoTexture_) {
            if (rhi::IResourceFactory* res = e.device()->resources()) {
                res->waitIdle();
                res->destroyTexture(logoTexture_);
            }
            logoTexture_ = 0; logoUiId_ = 0;
        }
#endif
#if AVER_MODULE_VOXI
        // Deregister before releasing: the device holds a bare pointer to the feature.
        if (voxiAttached_) { e.device()->removeRenderFeature(&voxiRenderer_); voxiAttached_ = false; }
        voxiRenderer_.shutdown();
#else
        (void)e;
#endif
        AVER_INFO("[Sandbox] shutdown");
    }
    void setFocusVoxi(bool b) { focusVoxi_ = b ? 4 : 0; } // --project-settings screenshot aid
    void setFocusScript(bool b) { focusScript_ = b ? 4 : 0; } // --new-script screenshot aid
    void setMsaaOverride(int n) { msaaOverride_ = n; }   // --msaa N
    void setGiOverride(int q, bool dbg) { giOverride_ = q; giDebugView_ = dbg; } // --gi / --gi-debug
    void setRtOverride(int q) { rtOverride_ = q; }                              // --rt
    void setMsOverride(bool on) { msOverride_ = on; }                           // --ms
    void setProbe(u32 x, u32 y) { probeX_ = x; probeY_ = y; }                    // --probe X Y
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
    }

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

        // ---------------- menu bar ----------------
        if (ImGui::BeginMainMenuBar()) {
            // Medium weight on the menu bar, matching Unreal's; 0.0f keeps the size already in use.
            if (fontMedium_) ImGui::PushFont(fontMedium_, 0.0f);
            ImGui::TextColored(ImVec4(0.95f,0.42f,0.13f,1),"AE");
            if (ImGui::BeginMenu("File")){ ImGui::MenuItem("New Level"); ImGui::MenuItem("Open Level..."); ImGui::MenuItem("Save Level"); ImGui::Separator(); if(ImGui::MenuItem("Exit")) e.requestExit(); ImGui::EndMenu(); }
            if (ImGui::BeginMenu("Edit")){
                ImGui::MenuItem("Undo","Ctrl+Z"); ImGui::MenuItem("Redo","Ctrl+Y"); ImGui::Separator();
                ImGui::MenuItem("Editor Preferences");
                if (ImGui::MenuItem("Project Settings...")) showProjectSettings_ = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Window")){ ImGui::MenuItem("World Outliner"); ImGui::MenuItem("Details"); ImGui::MenuItem("Content Browser"); ImGui::MenuItem("Output Log"); ImGui::Separator(); if (ImGui::MenuItem("Reset Layout")) dockBuilt_=false; ImGui::EndMenu(); }
            // Tools sits between Window and Build, where Unreal puts it.
            if (ImGui::BeginMenu("Tools")){
                const bool haveProject = project_.valid();
                if (ImGui::MenuItem("New C# Script...", nullptr, false, haveProject)) {
                    scriptName_[0] = '\0'; scriptError_.clear(); scriptResult_.clear();
                    openScriptModal_ = true;
                }
                // A disabled item with no explanation reads as a bug. Scripts are written into the
                // project's Content\Scripts, so with no project there is nowhere to put one.
                if (!haveProject && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Open or create a project first - scripts live in the\nproject's Content\\Scripts folder.");
                ImGui::EndMenu();
            }
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
        // Play controls, centred like Unreal's.
        {
            const f32 grpW = 200.0f*dpi_;
            ImGui::SameLine(std::fmax(ImGui::GetCursorPosX(), (wsize.x - grpW)*0.5f));
            ImGui::Button("Play"); ImGui::SameLine();
            ImGui::Button("Pause"); ImGui::SameLine();
            ImGui::Button("Stop");
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
            ImGuiID centre = dockId, right = 0, rightTop = 0, rightBottom = 0, bottom = 0;
            ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.22f, &right,  &centre);
            ImGui::DockBuilderSplitNode(right,  ImGuiDir_Down,  0.60f, &rightBottom, &rightTop);
            ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down,  0.26f, &bottom, &centre);
            ImGui::DockBuilderDockWindow("World Outliner",  rightTop);
            ImGui::DockBuilderDockWindow("Details",         rightBottom);
            ImGui::DockBuilderDockWindow("Content Browser", bottom);
            ImGui::DockBuilderDockWindow("Output Log",      bottom);
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
        buildProjectSettings();
        buildNewScriptModal();

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
        ImGui::End();
        ImGui::PopStyleVar(2);
#else
        (void)e;
#endif
    }

#if AVER_WITH_IMGUI
    // Docked panels. These are plain windows — the dock builder placed them, and the user can
    // re-dock, tab or float them freely from here on.
    void buildPanels(Engine& e) {
        ImGui::Begin("World Outliner");
        for (int i=0;i<(int)objects_.size();++i)
            if (ImGui::Selectable((std::string("  ")+objects_[i].name).c_str(), sel_==i)) sel_=i;
        ImGui::Separator();
        if (ImGui::Selectable("  Directional Light (Sun)", sel_==-2)) sel_=-2;
        if (ImGui::Selectable("  Sky + Atmosphere", sel_==-3)) sel_=-3;
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
                ImGui::SliderFloat("Metallic", &o.metallic, 0.0f, 1.0f);
                ImGui::SliderFloat("Roughness", &o.roughness, 0.02f, 1.0f);
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
        } else ImGui::TextDisabled("Select an actor in the World Outliner");
        ImGui::End();

        ImGui::Begin("Content Browser");
        // Where the mount points, even though nothing enumerates it yet: "no content" and "no
        // project" are different states and the panel used to show one message for both.
        if (project_.valid()) {
            ImGui::Text("%s", project_.name.c_str());
            ImGui::TextDisabled("Mounted at %s", project_.contentDir().c_str());
            if (!project_.startMap.empty()) ImGui::TextDisabled("Start map: %s", project_.startMap.c_str());
            ImGui::Separator();
        } else {
            ImGui::TextDisabled("No project loaded - nothing is mounted.");
            ImGui::Separator();
        }
        ImGui::TextDisabled("Static meshes (.ocmesh), materials and textures will appear here");
        ImGui::TextDisabled("once the asset pipeline lands (see docs/STATUS.md \xC2\xA7""9).");
        ImGui::End();

        ImGui::Begin("Output Log");
        ImGui::TextUnformatted("[INFO] Aver Engine 0.1 started");
        ImGui::Text("[INFO] Backend %s on %s", rhi::backendName(e.device()->backend()), e.device()->adapterName());
        ImGui::Text("[INFO] Editor DPI scale %.2f", dpi_);
        ImGui::TextDisabled("(log capture wiring is a TODO - this mirrors the console for now)");
        ImGui::End();
    }

    // Tools > New C# Script. Writes one file into the project's Content\Scripts and, the first
    // time, the .csproj that makes the folder open as a real project in an IDE.
    void buildNewScriptModal() {
        // --new-script (screenshot aid, like --project-settings). Deliberately gated on the SAME
        // predicate as the menu item, so a run with no project proves the item really is disabled
        // rather than merely looking it — headless capture cannot open a menu and click.
        if (focusScript_ > 0) { if (project_.valid()) openScriptModal_ = true; --focusScript_; }
        if (openScriptModal_) { ImGui::OpenPopup("New C# Script"); openScriptModal_ = false; }

        const ImGuiViewport* mv = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(mv->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(600.0f*dpi_, 0), ImGuiCond_Always);
        if (!ImGui::BeginPopupModal("New C# Script", nullptr, ImGuiWindowFlags_NoResize)) return;

        ImGui::TextUnformatted("Script name");
        ImGui::PushItemWidth(-1);
        const bool submitted = ImGui::InputText("##scriptname", scriptName_, sizeof scriptName_,
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();
        ImGui::TextDisabled("Becomes a C# class, so: letters, digits and underscores only.");

        ImGui::Spacing();
        ImGui::TextDisabled("Writes to %s", project_.scriptsDir().c_str());

        // Said here as well as in the generated file's header: a script that silently never runs
        // is the kind of thing someone discovers an hour later, by watching nothing happen.
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(0.95f,0.72f,0.25f,1),
            "This script will NOT run. The engine cannot host the CLR in-process yet "
            "(docs/STATUS.md \xC2\xA7""4d), so nothing loads or calls it. It compiles and is editable "
            "against the real Aver.Scripting API; execution is not wired up.");
        ImGui::PopTextWrapPos();
        ImGui::Separator();

        if (!scriptError_.empty())  ImGui::TextColored(ImVec4(0.93f,0.42f,0.38f,1), "%s", scriptError_.c_str());
        if (!scriptResult_.empty()) ImGui::TextColored(ImVec4(0.45f,0.85f,0.45f,1), "%s", scriptResult_.c_str());

        ImGui::Spacing();
        const bool create = ImGui::Button("Create Script", ImVec2(150.0f*dpi_, 0));
        if (create || submitted) {
            scriptError_.clear(); scriptResult_.clear();
            std::string path; bool madeCsproj = false;
            if (editor::createScript(project_, scriptName_, &path, &madeCsproj, &scriptError_)) {
                scriptResult_ = "Created " + path + (madeCsproj ? "  (+ Scripts.csproj)" : "");
                scriptName_[0] = '\0';
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(110.0f*dpi_, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
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
        if (f==sf) e.device()->requestCapture(px_, py_);
        if (f>sf && !capDone_){
            // The raw 8-bit codes as well as the rounded floats: at two decimal places a whole code
            // of movement can hide inside one printed digit, which is exactly how a one-code-wide
            // wobble in the GI path went unnoticed while a three-code one did not.
            f32 px[4]; if (e.device()->getCapture(px))
                AVER_INFO("[Sandbox] probe ({},{}) px ({:.2f},{:.2f},{:.2f}) raw ({},{},{})", px_, py_, px[0],px[1],px[2],
                          (int)(px[0]*255.0f+0.5f), (int)(px[1]*255.0f+0.5f), (int)(px[2]*255.0f+0.5f));
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
    // sun
    f32 sunAz_=-0.55f, sunAlt_=-0.45f, sunUp_=0.55f, sunColor_[3]={1.0f,0.96f,0.9f}, sunAmbient_=0.28f;
    // sky + atmosphere
    f32 skyZenith_[3]={0.19f,0.42f,0.78f}, skyHorizon_[3]={0.72f,0.80f,0.90f};
    f32 fogColor_[3]={0.70f,0.78f,0.88f}, fogDensity_=0.014f;
    bool capDone_=false;
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
    int  settingsPage_=1;            // 0 = Description, 1 = Rendering
    int  focusVoxi_=0;               // --project-settings: frames left to force the window open
    int  focusScript_=0;             // --new-script: ditto for the New C# Script modal
    int  msaaOverride_=0;            // --msaa N: apply a sample count at startup
    int  giOverride_=0;              // --gi: GI quality to apply at startup
    int  rtOverride_=0;              // --rt: ray tracing quality at startup
    bool msOverride_=false;          // --ms: force the mesh shader geometry path
    u32  probeX_=0, probeY_=0;       // --probe X Y: absolute capture pixel (0 = viewport centre)
    // Project browser + the project it produced. `browserActive_` is false for every automated
    // run, so the oracle never sees the start screen.
    editor::ProjectBrowser browser_;
    fmt::ProjectDesc project_;
    std::string projectPath_;        // <path>.ocproject given on the command line
    bool browserActive_=false;
    // The start screen's mark. Zero when the screen was never armed, or when logo.png was missing
    // or undecodable -- in which case the browser draws its fallback badge instead.
    rhi::TextureHandle logoTexture_=0;
    u64 logoUiId_=0;
    f32 logoAspect_=1.0f;
    bool openScriptModal_=false;     // Tools > New C# Script
    char scriptName_[96]={};
    std::string scriptError_, scriptResult_;
    bool worldSpace_=true;   // gizmo coordinate space toggle (display only for now)
    // Voxi GI volume placement: a cube around the default scene (floor is +/-40, cube at origin).
    bool giDebugView_=false; Vec3 giCenter_{0,0,8}; f32 giExtent_=44.0f;
#if AVER_MODULE_VOXI
    // Voxi's GPU side. Inert for now: it creates its resources and leaves them idle.
    voxi::VoxiRenderer voxiRenderer_;
    bool voxiAttached_=false;
#endif
    rhi::MeshHandle cubeMesh_=0; u32 cubeTris_=0; int spawnCount_=0;
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
    u64 frames=0; bool headless=false, focusVoxi=false, focusScript=false, startScreen=false; std::string beam, shot, project; Tool tool=Tool::Select; int msaa=0; int gi=0; int rt=0; bool giDbg=false, ms=false; u32 probeX=0, probeY=0;
    for (int i=1;i<argc;++i){
        if (!std::strcmp(argv[i],"--headless")) headless=true;
        else if (!std::strcmp(argv[i],"--project-settings")) focusVoxi=true;
        else if (!std::strcmp(argv[i],"--new-script")) focusScript=true;
        else if (!std::strcmp(argv[i],"--start-screen")) startScreen=true;
        else if (!std::strcmp(argv[i],"--msaa") && i+1<argc) msaa=std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i],"--gi")) gi=3;
        else if (!std::strcmp(argv[i],"--gi-debug")) { gi=3; giDbg=true; }
        else if (!std::strcmp(argv[i],"--rt")) rt=3;
        else if (!std::strcmp(argv[i],"--ms")) ms=true;
        else if (!std::strcmp(argv[i],"--probe") && i+2<argc) { probeX=(u32)std::atoi(argv[++i]); probeY=(u32)std::atoi(argv[++i]); }
        else if (!std::strcmp(argv[i],"--frames") && i+1<argc) frames=std::strtoull(argv[++i],nullptr,10);
        else if (!std::strcmp(argv[i],"--screenshot") && i+1<argc) shot=argv[++i];
        else if (!std::strcmp(argv[i],"--tool") && i+1<argc) {
            const char* t=argv[++i];
            tool = !std::strcmp(t,"move")?Tool::Move : !std::strcmp(t,"rotate")?Tool::Rotate :
                   !std::strcmp(t,"scale")?Tool::Scale : Tool::Select;
        }
        else if (argv[i][0]!='-') { if (isOcproject(argv[i])) project=argv[i]; else beam=argv[i]; }
    }
    auto* app = new SandboxApp(frames, headless, beam, shot, tool);
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
    app->setFocusScript(focusScript);
    app->setMsaaOverride(msaa);
    app->setGiOverride(gi, giDbg);
    app->setRtOverride(rt);
    app->setMsOverride(ms);
    app->setProbe(probeX, probeY);
    return app;
}

} // namespace aver
