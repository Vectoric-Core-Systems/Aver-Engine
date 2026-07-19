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
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace aver {

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
    v.push_back({0,0,0.03f, 0.80f,0.25f,0.25f}); v.push_back({ext,0,0.03f, 0.80f,0.25f,0.25f}); // +X
    v.push_back({0,0,0.03f, 0.28f,0.72f,0.30f}); v.push_back({0,ext,0.03f, 0.28f,0.72f,0.30f}); // +Y
}
static void buildGizmo(std::vector<rhi::LineVertex>& v, f32 len) {
    v.push_back({0,0,0, 0.92f,0.22f,0.22f}); v.push_back({len,0,0, 0.92f,0.22f,0.22f});
    v.push_back({0,0,0, 0.24f,0.85f,0.28f}); v.push_back({0,len,0, 0.24f,0.85f,0.28f});
    v.push_back({0,0,0, 0.32f,0.52f,1.0f});  v.push_back({0,0,len, 0.32f,0.52f,1.0f});
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
    SandboxApp(u64 maxFrames, bool headless, std::string beamPath, std::string shot)
        : maxFrames_(maxFrames), headless_(headless), beamPath_(std::move(beamPath)), shot_(std::move(shot)) {}

    BootConfig config() const override {
        BootConfig c; c.windowTitle="Aver Engine \xE2\x80\x94 Editor"; c.windowWidth=1600; c.windowHeight=900;
        c.maxFrames=maxFrames_; c.headless=headless_; return c;
    }

    void onInit(Engine& e) override {
        AVER_INFO("[Sandbox] backend={} adapter='{}'", rhi::backendName(e.device()->backend()), e.device()->adapterName());
#if AVER_WITH_IMGUI
        if (e.device()->uiActive()) applyUnrealStyle();
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
        std::vector<rhi::LineVertex> gz; buildGizmo(gz, 1.8f);
        gizmoMesh_ = e.device()->createLineMesh(gz.data(), (u32)gz.size());

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
                yaw_   += io.MouseDelta.x * 0.005f;
                pitch_ -= io.MouseDelta.y * 0.005f;
                pitch_ = pitch_ < -1.54f ? -1.54f : (pitch_ > 1.54f ? 1.54f : pitch_);
                if (io.MouseWheel != 0.0f) {
                    flySpeed_ *= (1.0f + io.MouseWheel * 0.15f);
                    flySpeed_ = flySpeed_ < 0.5f ? 0.5f : (flySpeed_ > 200.0f ? 200.0f : flySpeed_);
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
        invVP_ = invVP; eye_ = camPos_;

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
        if (sel_>=0 && sel_<(int)objects_.size()) {
            Mat4 g = Mat4::translation(objects_[sel_].pos);
            e.device()->drawLines(gizmoMesh_, &g.m[0][0]);
        }
        buildUI(e);
        captureCheck(e);
    }

    void onShutdown(Engine&) override { AVER_INFO("[Sandbox] shutdown"); }

private:
    static f32 viewAspect(Engine& e){ return (e.window()&&e.window()->height())?(f32)e.window()->width()/e.window()->height():1.777f; }
    Vec3 camForward() const {
        return Vec3{ std::cos(pitch_)*std::cos(yaw_), std::cos(pitch_)*std::sin(yaw_), std::sin(pitch_) };
    }

    void handleManip(Engine& e) {
#if AVER_WITH_IMGUI
        if (!e.device()->uiActive()) return;
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsKeyPressed(ImGuiKey_1)) tool_=Tool::Select;
        if (ImGui::IsKeyPressed(ImGuiKey_2)) tool_=Tool::Move;
        if (ImGui::IsKeyPressed(ImGuiKey_3)) tool_=Tool::Rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_4)) tool_=Tool::Scale;
        if (tool_==Tool::Select) {
            if (!io.WantCaptureMouse && ImGui::IsMouseClicked(0)) pick(e, io);
            return;
        }
        if (io.WantCaptureMouse || !io.MouseDown[0]) return;
        if (sel_<0 || sel_>=(int)objects_.size()) return;
        MeshObj& o = objects_[sel_]; f32 dx=io.MouseDelta.x, dy=io.MouseDelta.y;
        if (tool_==Tool::Move){ o.pos.x+=dx*0.02f; o.pos.z-=dy*0.02f; }
        else if (tool_==Tool::Rotate){ o.rotDeg.z+=dx*0.5f; o.rotDeg.x+=dy*0.5f; }
        else if (tool_==Tool::Scale){ f32 s=1+dx*0.005f; o.scale=o.scale*(s>0.05f?s:0.05f); }
#else
        (void)e;
#endif
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

        const f32 top = ImGui::GetFrameHeight();
        // Toolbar (top strip): tool/selection modes — Select first.
        ImGui::SetNextWindowPos(ImVec2(0, top)); ImGui::SetNextWindowSize(ImVec2(W, 40));
        ImGui::Begin("##toolbar", nullptr, ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoScrollbar);
        for (int i=0;i<4;++i){
            const bool on = (int)tool_==i;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f,0.42f,0.13f,0.85f));
            if (ImGui::Button(kToolNames[i], ImVec2(74,24))) tool_=(Tool)i;
            if (on) ImGui::PopStyleColor();
            ImGui::SameLine();
        }
        ImGui::Checkbox("Grid", &showGrid_); ImGui::SameLine();
        ImGui::Checkbox("Wireframe", &wireframe_); ImGui::SameLine();
        ImGui::TextDisabled("| RMB: fly (WASD/QE)  wheel: speed  MMB: pan  F: focus   |   1-4 tools, click: select, L-drag: %s", kToolNames[(int)tool_]);
        ImGui::End();

        const f32 y0 = top+40, rightW=320, bottomH=150;
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
        ImGui::Text("Aver Engine 0.1  |  %s  |  %s", rhi::backendName(e.device()->backend()), e.device()->adapterName());
        ImGui::Text("FPS %.0f (%.2f ms)   objects %zu   tool %s   frame %llu",
                    dt>1e-6f?1.f/dt:0.f, dt*1000.f, objects_.size(), kToolNames[(int)tool_], (unsigned long long)e.time().frame);
        ImGui::End();

        // Viewport HUD (bottom-left overlay)
        ImGui::SetNextWindowPos(ImVec2(10, H-bottomH-30)); ImGui::SetNextWindowBgAlpha(0.35f);
        ImGui::Begin("##hud", nullptr, ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoInputs);
        ImGui::Text("Perspective  |  Lit  |  %s", kToolNames[(int)tool_]);
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
    std::vector<MeshObj> objects_;
    int sel_ = 1;
    Tool tool_ = Tool::Select;
    // Free-fly editor camera (Unreal-style): position + yaw/pitch, no auto-orbit.
    Vec3 camPos_{7.0f, 7.0f, 4.5f};
    f32 yaw_ = 0.0f, pitch_ = 0.0f, flySpeed_ = 8.0f;
    bool flying_ = false;
    // sun
    f32 sunAz_=0.35f, sunAlt_=0.4f, sunUp_=0.85f, sunColor_[3]={1.0f,0.96f,0.9f}, sunAmbient_=0.28f;
    // sky + atmosphere
    f32 skyZenith_[3]={0.19f,0.42f,0.78f}, skyHorizon_[3]={0.72f,0.80f,0.90f};
    f32 fogColor_[3]={0.70f,0.78f,0.88f}, fogDensity_=0.014f;
    bool capDone_=false;
    // editor viewport aids
    rhi::LineHandle gridMesh_=0, gizmoMesh_=0;
    bool showGrid_=true, wireframe_=false;
    Mat4 invVP_; Vec3 eye_{0,0,0};
};

Application* createApplication(int argc, char** argv) {
    u64 frames=0; bool headless=false; std::string beam, shot;
    for (int i=1;i<argc;++i){
        if (!std::strcmp(argv[i],"--headless")) headless=true;
        else if (!std::strcmp(argv[i],"--frames") && i+1<argc) frames=std::strtoull(argv[++i],nullptr,10);
        else if (!std::strcmp(argv[i],"--screenshot") && i+1<argc) shot=argv[++i];
        else if (argv[i][0]!='-') beam=argv[i];
    }
    return new SandboxApp(frames, headless, beam, shot);
}

} // namespace aver
