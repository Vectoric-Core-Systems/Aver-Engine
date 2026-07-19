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

enum class Tool { Select, Move, Rotate, Scale };
static const char* kToolNames[4] = {"Select", "Move", "Rotate", "Scale"};

struct MeshObj {
    std::string name;
    rhi::MeshHandle mesh = 0;
    u32 tris = 0;
    Vec3 pos{0,0,0}, rotDeg{0,0,0}, scale{1,1,1};
    f32 color[4] = {0.8f,0.4f,0.25f,1};
    bool visible = true;
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
        objects_.push_back(floor);
        MeshObj c; c.name="Cube"; c.mesh=cube; c.tris=(u32)ci.size()/3; c.pos=Vec3{0,0,1};
        c.color[0]=0.85f; c.color[1]=0.36f; c.color[2]=0.22f;
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
                objects_.push_back(cg);
            }
        }
        sel_ = 1; // the Cube
        azimuth_=0.9f; elevation_=0.35f; distScale_=1.0f;
    }

    void onUpdate(Engine& e, const Timestep& t) override {
        bool over = e.device()->uiWantsMouse();
#if AVER_WITH_IMGUI
        if (e.device()->uiActive()) {
            const ImGuiIO& io = ImGui::GetIO();
            if (!over) {
                if (io.MouseDown[1]) { azimuth_ -= io.MouseDelta.x*0.01f; elevation_ += io.MouseDelta.y*0.01f; }
                if (io.MouseWheel != 0) distScale_ *= (1.0f - io.MouseWheel*0.1f);
            }
            elevation_ = elevation_<0.02f?0.02f:(elevation_>1.5f?1.5f:elevation_);
            distScale_ = distScale_<0.2f?0.2f:(distScale_>6.0f?6.0f:distScale_);
            if (autoOrbit_ && !io.MouseDown[0] && !io.MouseDown[1]) azimuth_ += t.dt*0.25f;
        } else
#endif
        { if (autoOrbit_) azimuth_ += t.dt*0.25f; }

        const Vec3 center{0,0,1};
        const f32 dist = 8.0f*distScale_;
        const f32 ch = std::cos(elevation_);
        const Vec3 eye = center + Vec3{std::cos(azimuth_)*ch, std::sin(azimuth_)*ch, std::sin(elevation_)}*dist;
        const f32 aspect = viewAspect(e);
        const Mat4 view = Mat4::lookAtLH(eye, center, Vec3{0,0,1});
        const Mat4 proj = Mat4::perspectiveLH(radians(52.0f), aspect, 0.1f, 4000.0f);
        const Mat4 viewProj = view * proj;
        const Mat4 invVP = viewProj.inverse();
        e.device()->setCamera(&viewProj.m[0][0], &invVP.m[0][0], &eye.x);

        const Vec3 ld = Vec3{sunAz_, sunAlt_, sunUp_}.getSafeNormal();
        e.device()->setLight(&ld.x, sunColor_, sunAmbient_);
        e.device()->setSky(true, skyZenith_, skyHorizon_, fogColor_, fogDensity_);
        e.device()->setClearColor(skyHorizon_[0], skyHorizon_[1], skyHorizon_[2], 1);
    }

    void onRender(Engine& e) override {
        handleManip(e);
        for (int i=0;i<(int)objects_.size();++i) {
            MeshObj& o = objects_[i];
            if (!o.visible) continue;
            Transform tr; tr.position=o.pos; tr.rotation=quatFromEulerDeg(o.rotDeg); tr.scale=o.scale;
            Mat4 w = tr.toMatrix();
            f32 col[4]={o.color[0],o.color[1],o.color[2],1};
            if (i==sel_) for (int k=0;k<3;++k) col[k]=std::fmin(1.0f,col[k]*1.3f+0.10f);
            e.device()->drawMesh(o.mesh, &w.m[0][0], col);
        }
        buildUI(e);
        captureCheck(e);
    }

    void onShutdown(Engine&) override { AVER_INFO("[Sandbox] shutdown"); }

private:
    static f32 viewAspect(Engine& e){ return (e.window()&&e.window()->height())?(f32)e.window()->width()/e.window()->height():1.777f; }

    void handleManip(Engine& e) {
#if AVER_WITH_IMGUI
        if (!e.device()->uiActive()) return;
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsKeyPressed(ImGuiKey_1)) tool_=Tool::Select;
        if (ImGui::IsKeyPressed(ImGuiKey_2)) tool_=Tool::Move;
        if (ImGui::IsKeyPressed(ImGuiKey_3)) tool_=Tool::Rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_4)) tool_=Tool::Scale;
        if (io.WantCaptureMouse || !io.MouseDown[0] || tool_==Tool::Select) return;
        if (sel_<0 || sel_>=(int)objects_.size()) return;
        MeshObj& o = objects_[sel_]; f32 dx=io.MouseDelta.x, dy=io.MouseDelta.y;
        if (tool_==Tool::Move){ o.pos.x+=dx*0.02f; o.pos.z-=dy*0.02f; }
        else if (tool_==Tool::Rotate){ o.rotDeg.z+=dx*0.5f; o.rotDeg.x+=dy*0.5f; }
        else if (tool_==Tool::Scale){ f32 s=1+dx*0.005f; o.scale=o.scale*(s>0.05f?s:0.05f); }
#else
        (void)e;
#endif
    }

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
            if (ImGui::BeginMenu("View")){ ImGui::MenuItem("Auto-orbit",nullptr,&autoOrbit_); ImGui::EndMenu(); }
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
        ImGui::TextDisabled("| keys 1-4   L-drag: %s   R-drag: orbit   wheel: zoom", kToolNames[(int)tool_]);
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
            ImGui::ColorEdit3("Color", o.color); ImGui::Checkbox("Visible", &o.visible);
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
    bool autoOrbit_ = true;
    f32 azimuth_=0.9f, elevation_=0.35f, distScale_=1.0f;
    // sun
    f32 sunAz_=0.35f, sunAlt_=0.4f, sunUp_=0.85f, sunColor_[3]={1.0f,0.96f,0.9f}, sunAmbient_=0.28f;
    // sky + atmosphere
    f32 skyZenith_[3]={0.19f,0.42f,0.78f}, skyHorizon_[3]={0.72f,0.80f,0.90f};
    f32 fogColor_[3]={0.70f,0.78f,0.88f}, fogDensity_=0.014f;
    bool capDone_=false;
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
