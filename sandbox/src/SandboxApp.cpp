#include "aver/runtime/EntryPoint.hpp"
#include "aver/platform/Window.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcBeam.hpp"

#if AVER_WITH_IMGUI
#include "imgui.h"
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
    for (const Face& f : faces) {
        const u32 b = static_cast<u32>(v.size());
        for (int k = 0; k < 4; ++k) { const f32* c = p[f.c[k]]; v.push_back({cx+c[0],cy+c[1],cz+c[2],f.n[0],f.n[1],f.n[2]}); }
        idx.push_back(b); idx.push_back(b+1); idx.push_back(b+2); idx.push_back(b); idx.push_back(b+2); idx.push_back(b+3);
    }
}
static void appendGround(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx, f32 s) {
    const u32 b = static_cast<u32>(v.size());
    v.push_back({-s,-s,0,0,0,1}); v.push_back({s,-s,0,0,0,1}); v.push_back({s,s,0,0,0,1}); v.push_back({-s,s,0,0,0,1});
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

    void onInit(Engine& e) override {
        AVER_INFO("[Sandbox] backend={} adapter='{}'", rhi::backendName(e.device()->backend()), e.device()->adapterName());
#if AVER_WITH_IMGUI
        if (e.device()->uiActive()) {
            applyUnrealStyle();
            dpi_ = e.window() ? e.window()->dpiScale() : 1.0f;
            if (dpi_ > 1.01f) { ImGui::GetStyle().ScaleAllSizes(dpi_); ImGui::GetIO().FontGlobalScale = dpi_; }
            AVER_INFO("[Sandbox] DPI scale {:.2f}", dpi_);
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
            const ImGuiIO& io = ImGui::GetIO();
            const bool overUI = io.WantCaptureMouse;

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
        const Vec3 fwd = camForward();
        const f32 aspect = viewAspect(e);
        const Mat4 view = Mat4::lookAtLH(camPos_, camPos_ + fwd, Vec3{0,0,1});
        const Mat4 proj = Mat4::perspectiveLH(radians(60.0f), aspect, 0.05f, 5000.0f);
        const Mat4 viewProj = view * proj;
        const Mat4 invVP = viewProj.inverse();
        e.device()->setCamera(&viewProj.m[0][0], &invVP.m[0][0], &camPos_.x);
        invVP_ = invVP; viewProj_ = viewProj; eye_ = camPos_;

        const Vec3 ld = Vec3{sunAz_, sunAlt_, sunUp_}.getSafeNormal();
        e.device()->setLight(&ld.x, sunColor_, sunAmbient_);
        e.device()->setSky(true, skyZenith_, skyHorizon_, fogColor_, fogDensity_);
        e.device()->setClearColor(skyHorizon_[0], skyHorizon_[1], skyHorizon_[2], 1);
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

    void onShutdown(Engine&) override { AVER_INFO("[Sandbox] shutdown"); }

private:
    static f32 viewAspect(Engine& e){ return (e.window()&&e.window()->height())?(f32)e.window()->width()/e.window()->height():1.777f; }
    Vec3 camForward() const {
        return Vec3{ std::cos(pitch_)*std::cos(yaw_), std::cos(pitch_)*std::sin(yaw_), std::sin(pitch_) };
    }
    bool movableSelected() const { return sel_ >= 0 && sel_ < (int)objects_.size(); }
    f32 gizmoLen(const Vec3& origin) const { f32 L = dist(eye_, origin) * 0.17f; return L < 0.5f ? 0.5f : L; }

    // Project a world point to screen pixels (row-vector clip = p * viewProj).
    bool project(const Vec3& wp, f32& sx, f32& sy) const {
        const Mat4& m = viewProj_;
        const f32 x = wp.x*m.m[0][0]+wp.y*m.m[1][0]+wp.z*m.m[2][0]+m.m[3][0];
        const f32 y = wp.x*m.m[0][1]+wp.y*m.m[1][1]+wp.z*m.m[2][1]+m.m[3][1];
        const f32 w = wp.x*m.m[0][3]+wp.y*m.m[1][3]+wp.z*m.m[2][3]+m.m[3][3];
        if (w <= 1e-4f) return false;
        sx = (x / w * 0.5f + 0.5f) * W_;
        sy = (1.0f - (y / w * 0.5f + 0.5f)) * H_;
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
            const f32 wpp = 2.0f * std::tan(radians(30.0f)) * dist(eye_, o.pos) / (H_ > 1 ? H_ : 900.0f);
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
        if (!e.device()->uiActive()) return;
        const ImGuiIO& io = ImGui::GetIO();
        W_ = e.window()?(f32)e.window()->width():1600.f;
        H_ = e.window()?(f32)e.window()->height():900.f;

        if (!io.WantCaptureKeyboard) {
            if (ImGui::IsKeyPressed(ImGuiKey_1)) tool_=Tool::Select;
            if (ImGui::IsKeyPressed(ImGuiKey_2)) tool_=Tool::Move;
            if (ImGui::IsKeyPressed(ImGuiKey_3)) tool_=Tool::Rotate;
            if (ImGui::IsKeyPressed(ImGuiKey_4)) tool_=Tool::Scale;
        }
        const f32 mx=io.MousePos.x, my=io.MousePos.y;

        // Hover highlight when idle over a handle.
        hoverAxis_ = -1;
        if (tool_!=Tool::Select && movableSelected() && !dragging_ && !io.WantCaptureMouse)
            hoverAxis_ = pickAxis(objects_[sel_].pos, gizmoLen(objects_[sel_].pos), mx, my);

        if (ImGui::IsMouseClicked(0) && !io.WantCaptureMouse) {
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
        const f32 W = e.window()?(f32)e.window()->width():1600.f;
        const f32 H = e.window()?(f32)e.window()->height():900.f;
        const f32 nx = io.MousePos.x / W * 2.f - 1.f;
        const f32 ny = 1.f - io.MousePos.y / H * 2.f;
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
        const f32 W = e.window()?(f32)e.window()->width():1600.f;
        const f32 H = e.window()?(f32)e.window()->height():900.f;

        if (ImGui::BeginMainMenuBar()) {
            ImGui::TextColored(ImVec4(0.95f,0.42f,0.13f,1),"AE"); ImGui::TextUnformatted("Aver Engine");
            ImGui::Separator();
            if (ImGui::BeginMenu("File")){ if(ImGui::MenuItem("New Level")) {} if(ImGui::MenuItem("Exit")) e.requestExit(); ImGui::EndMenu(); }
            if (ImGui::BeginMenu("Edit")){ ImGui::MenuItem("Undo"); ImGui::MenuItem("Redo"); ImGui::EndMenu(); }
            if (ImGui::BeginMenu("View")){ ImGui::MenuItem("Grid",nullptr,&showGrid_); ImGui::MenuItem("Wireframe",nullptr,&wireframe_); ImGui::EndMenu(); }
            ImGui::EndMainMenuBar();
        }

        const f32 menuH = ImGui::GetFrameHeight();
        const f32 icon = 30.0f*dpi_;
        const f32 toolbarH = icon + 14.0f*dpi_;
        const f32 gap = 8.0f*dpi_, tiny = 2.0f*dpi_, caretW = 15.0f*dpi_;

        // Transform toolbar: icon tools tucked in the top-middle, just above the viewport.
        ImGui::SetNextWindowPos(ImVec2(0, menuH)); ImGui::SetNextWindowSize(ImVec2(W, toolbarH));
        ImGui::Begin("##toolbar", nullptr, ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|
                     ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoNavFocus);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        auto toolBtn = [&](const char* id, int kind, bool active)->bool {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton(id, ImVec2(icon, icon));
            const bool hov = ImGui::IsItemHovered(), clk = ImGui::IsItemClicked();
            const ImU32 bg = active ? IM_COL32(232,110,35,235) : (hov ? IM_COL32(74,76,82,255) : IM_COL32(48,49,54,255));
            dl->AddRectFilled(p, ImVec2(p.x+icon,p.y+icon), bg, 5.0f);
            dl->AddRect(p, ImVec2(p.x+icon,p.y+icon), IM_COL32(0,0,0,120), 5.0f);
            drawToolGlyph(dl, p, icon, kind, IM_COL32(236,237,240,255));
            return clk;
        };
        auto caretBtn = [&](const char* id, bool on)->bool {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton(id, ImVec2(caretW, icon));
            const bool hov = ImGui::IsItemHovered(), clk = ImGui::IsItemClicked();
            if (hov) dl->AddRectFilled(p, ImVec2(p.x+caretW,p.y+icon), IM_COL32(74,76,82,255), 4.0f);
            const ImVec2 c(p.x+caretW*0.5f, p.y+icon*0.5f); const f32 s=3.0f*dpi_;
            const ImU32 col = on ? IM_COL32(232,150,60,255) : IM_COL32(190,191,195,255);
            dl->AddTriangleFilled(ImVec2(c.x-s,c.y-s*0.6f), ImVec2(c.x+s,c.y-s*0.6f), ImVec2(c.x,c.y+s*0.8f), col);
            return clk;
        };

        const f32 groupW = icon + 3.0f*(gap + icon + tiny + caretW);
        f32 startX = (W - groupW) * 0.5f; if (startX < 8.0f*dpi_) startX = 8.0f*dpi_;
        ImGui::SetCursorPosY((toolbarH - icon) * 0.5f);
        ImGui::SetCursorPosX(startX);

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

        if (ImGui::BeginPopup("snapMove")) {
            ImGui::Checkbox("Grid snap (position)", &snapMove_); ImGui::Separator();
            const f32 opts[] = {0.1f,0.25f,0.5f,1,2,5,10,50,100};
            for (f32 f : opts){ char b[24]; std::snprintf(b,sizeof b,"%g units", f); if (ImGui::Selectable(b, moveSnap_==f)){ moveSnap_=f; snapMove_=true; } }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("snapRot")) {
            ImGui::Checkbox("Angle snap (rotation)", &snapRot_); ImGui::Separator();
            const f32 opts[] = {1,5,10,15,30,45,90};
            for (f32 f : opts){ char b[24]; std::snprintf(b,sizeof b,"%g\xC2\xB0", f); if (ImGui::Selectable(b, rotSnap_==f)){ rotSnap_=f; snapRot_=true; } }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("snapScale")) {
            ImGui::Checkbox("Scale snap", &snapScale_); ImGui::Separator();
            const f32 opts[] = {0.05f,0.1f,0.25f,0.5f,1};
            for (f32 f : opts){ char b[24]; std::snprintf(b,sizeof b,"%g", f); if (ImGui::Selectable(b, scaleSnap_==f)){ scaleSnap_=f; snapScale_=true; } }
            ImGui::EndPopup();
        }

        // Grid / Wireframe toggles, right-aligned in the same strip (only if they clear the
        // centred tool group; otherwise they stay reachable from the View menu).
        if (startX + groupW < W - 240.0f*dpi_) {
            ImGui::SetCursorPosX(W - 220.0f*dpi_);
            ImGui::SetCursorPosY((toolbarH - ImGui::GetFrameHeight()) * 0.5f);
            ImGui::Checkbox("Grid", &showGrid_); ImGui::SameLine(); ImGui::Checkbox("Wireframe", &wireframe_);
        }
        ImGui::End();

        const f32 y0 = menuH + toolbarH, rightW = 320.0f*dpi_, bottomH = 150.0f*dpi_;
        // World Outliner (top-right)
        ImGui::SetNextWindowPos(ImVec2(W-rightW, y0)); ImGui::SetNextWindowSize(ImVec2(rightW,(H-y0-bottomH)*0.5f));
        ImGui::Begin("World Outliner");
        for (int i=0;i<(int)objects_.size();++i)
            if (ImGui::Selectable((std::string("  ")+objects_[i].name).c_str(), sel_==i)) sel_=i;
        ImGui::Separator();
        if (ImGui::Selectable("  Directional Light (Sun)", sel_==-2)) sel_=-2;
        if (ImGui::Selectable("  Sky + Atmosphere", sel_==-3)) sel_=-3;
        ImGui::End();

        // Details (bottom-right)
        ImGui::SetNextWindowPos(ImVec2(W-rightW, y0+(H-y0-bottomH)*0.5f)); ImGui::SetNextWindowSize(ImVec2(rightW,(H-y0-bottomH)*0.5f));
        ImGui::Begin("Details");
        if (sel_>=0 && sel_<(int)objects_.size()){
            MeshObj& o=objects_[sel_]; ImGui::TextUnformatted(o.name.c_str()); ImGui::Separator();
            ImGui::DragFloat3("Location", &o.pos.x, 0.05f);
            ImGui::DragFloat3("Rotation", &o.rotDeg.x, 1.0f);
            ImGui::DragFloat3("Scale", &o.scale.x, 0.01f, 0.02f, 100.f);
            ImGui::ColorEdit3("Color", o.color);
            ImGui::SliderFloat("Metallic", &o.metallic, 0.0f, 1.0f);
            ImGui::SliderFloat("Roughness", &o.roughness, 0.02f, 1.0f);
            ImGui::Checkbox("Visible", &o.visible);
        } else if (sel_==-2){
            ImGui::TextUnformatted("Directional Light"); ImGui::Separator();
            ImGui::SliderFloat("Azimuth", &sunAz_, -1, 1); ImGui::SliderFloat("Altitude", &sunAlt_, -1, 1); ImGui::SliderFloat("Up", &sunUp_, 0.05f, 2);
            ImGui::ColorEdit3("Color", sunColor_); ImGui::SliderFloat("Ambient", &sunAmbient_, 0, 1);
        } else if (sel_==-3){
            ImGui::TextUnformatted("Sky + Atmosphere"); ImGui::Separator();
            ImGui::ColorEdit3("Zenith", skyZenith_); ImGui::ColorEdit3("Horizon", skyHorizon_);
            ImGui::ColorEdit3("Fog", fogColor_); ImGui::SliderFloat("Fog density", &fogDensity_, 0, 0.06f, "%.4f");
        } else ImGui::TextDisabled("Select something in the Outliner");
        ImGui::End();

        // Output Log (bottom strip)
        ImGui::SetNextWindowPos(ImVec2(0, H-bottomH)); ImGui::SetNextWindowSize(ImVec2(W, bottomH));
        ImGui::Begin("Output Log");
        const f32 dt=e.time().dt;
        ImGui::Text("Aver Engine 0.1  |  %s  |  %s  |  DPI %.0f%%", rhi::backendName(e.device()->backend()), e.device()->adapterName(), dpi_*100.f);
        ImGui::Text("FPS %.0f (%.2f ms)   objects %zu   tool %s   frame %llu",
                    dt>1e-6f?1.f/dt:0.f, dt*1000.f, objects_.size(), kToolNames[(int)tool_], (unsigned long long)e.time().frame);
        ImGui::End();

        // Viewport HUD (bottom-left overlay)
        ImGui::SetNextWindowPos(ImVec2(10, H-bottomH-30*dpi_)); ImGui::SetNextWindowBgAlpha(0.35f);
        ImGui::Begin("##hud", nullptr, ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoInputs);
        ImGui::Text("Perspective | Lit | %s   \xE2\x80\x94   RMB fly (WASD/QE)  wheel speed  MMB pan  F focus   1-4 tools", kToolNames[(int)tool_]);
        ImGui::End();
#else
        (void)e;
#endif
    }

    void captureCheck(Engine& e) {
        const u64 f = e.time().frame; u32 ww=1600,wh=900;
        if (e.window()){ ww=e.window()->width(); wh=e.window()->height(); }
        const u64 sf = maxFrames_>8?maxFrames_-3:4;
        if (f==sf) e.device()->requestCapture(ww/2, wh/2);
        if (f>sf && !capDone_){
            f32 px[4]; if (e.device()->getCapture(px)) AVER_INFO("[Sandbox] centre px ({:.2f},{:.2f},{:.2f})", px[0],px[1],px[2]);
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
    f32 sunAz_=0.35f, sunAlt_=0.4f, sunUp_=0.85f, sunColor_[3]={1.0f,0.96f,0.9f}, sunAmbient_=0.28f;
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
    f32 dpi_=1.0f, W_=1600, H_=900;
    Mat4 invVP_, viewProj_; Vec3 eye_{0,0,0};
};

Application* createApplication(int argc, char** argv) {
    u64 frames=0; bool headless=false; std::string beam, shot; Tool tool=Tool::Select;
    for (int i=1;i<argc;++i){
        if (!std::strcmp(argv[i],"--headless")) headless=true;
        else if (!std::strcmp(argv[i],"--frames") && i+1<argc) frames=std::strtoull(argv[++i],nullptr,10);
        else if (!std::strcmp(argv[i],"--screenshot") && i+1<argc) shot=argv[++i];
        else if (!std::strcmp(argv[i],"--tool") && i+1<argc) {
            const char* t=argv[++i];
            tool = !std::strcmp(t,"move")?Tool::Move : !std::strcmp(t,"rotate")?Tool::Rotate :
                   !std::strcmp(t,"scale")?Tool::Scale : Tool::Select;
        }
        else if (argv[i][0]!='-') beam=argv[i];
    }
    return new SandboxApp(frames, headless, beam, shot, tool);
}

} // namespace aver
