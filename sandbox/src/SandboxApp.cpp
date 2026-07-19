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

static void appendBox(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx,
                      f32 cx, f32 cy, f32 cz, f32 h) {
    const f32 p[8][3] = {{-h,-h,-h},{h,-h,-h},{h,h,-h},{-h,h,-h},{-h,-h,h},{h,-h,h},{h,h,h},{-h,h,h}};
    struct Face { f32 n[3]; int c[4]; };
    const Face faces[6] = {
        {{1,0,0},{1,2,6,5}}, {{-1,0,0},{0,4,7,3}}, {{0,1,0},{3,7,6,2}},
        {{0,-1,0},{0,1,5,4}}, {{0,0,1},{4,5,6,7}}, {{0,0,-1},{0,3,2,1}}};
    for (const Face& f : faces) {
        const u32 base = static_cast<u32>(v.size());
        for (int k = 0; k < 4; ++k) {
            const f32* c = p[f.c[k]];
            v.push_back({cx + c[0], cy + c[1], cz + c[2], f.n[0], f.n[1], f.n[2]});
        }
        idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
        idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
    }
}

static Quat quatFromEulerDeg(const Vec3& e) {
    Quat qx = Quat::fromAxisAngle({1, 0, 0}, radians(e.x));
    Quat qy = Quat::fromAxisAngle({0, 1, 0}, radians(e.y));
    Quat qz = Quat::fromAxisAngle({0, 0, 1}, radians(e.z));
    return (qz * qy * qx).normalized();
}

enum class Tool { Select, Move, Rotate, Scale };
static const char* kToolNames[4] = {"Select", "Move", "Rotate", "Scale"};

struct SceneObject {
    std::string name;
    Vec3 pos{0, 0, 0};
    Vec3 rotDeg{0, 0, 0};
    Vec3 scale{1, 1, 1};
    f32 color[4] = {0.8f, 0.4f, 0.25f, 1.0f};
    bool visible = true;
};

class SandboxApp final : public Application {
public:
    SandboxApp(u64 maxFrames, bool headless, std::string beamPath, std::string screenshotPath)
        : maxFrames_(maxFrames), headless_(headless), beamPath_(std::move(beamPath)),
          screenshotPath_(std::move(screenshotPath)) {}

    BootConfig config() const override {
        BootConfig c;
        c.windowTitle = "Aver Engine \xE2\x80\x94 Editor";
        c.windowWidth = 1440; c.windowHeight = 810;
        c.maxFrames = maxFrames_; c.headless = headless_;
        return c;
    }

    void onInit(Engine& e) override {
        AVER_INFO("[Sandbox] init on RHI backend={} adapter='{}'",
                  rhi::backendName(e.device()->backend()), e.device()->adapterName());
        const f32 in[4] = {0.1f, 0.2f, 0.3f, 1.0f}; f32 out[4] = {};
        if (e.device()->selfTest(in, out))
            AVER_INFO("[Sandbox] GPU self-test read ({:.3f},{:.3f},{:.3f})", out[0], out[1], out[2]);

        std::vector<rhi::MeshVertex> verts; std::vector<u32> indices;
        bool cage = false;
        if (!beamPath_.empty()) {
            fmt::OcBeamData beam; std::string err;
            if (fmt::loadOcbeam(beamPath_, beam, &err) && !beam.nodes.empty()) {
                AABB b; b.min = b.max = Vec3{beam.nodes[0].x, beam.nodes[0].y, beam.nodes[0].z};
                for (const auto& n : beam.nodes) b.expand(Vec3{n.x, n.y, n.z});
                const Vec3 ext = b.extent();
                const f32 dot = (ext.x + ext.y + ext.z) * 0.004f + 1.0f;
                for (const auto& n : beam.nodes) appendBox(verts, indices, n.x, n.y, n.z, dot);
                meshRadius_ = ext.x; meshRadius_ = ext.y > meshRadius_ ? ext.y : meshRadius_; meshRadius_ = ext.z > meshRadius_ ? ext.z : meshRadius_;
                center_ = b.center();
                AVER_INFO("[Sandbox] loaded cage '{}' ({} nodes, {} verts)", beamPath_, beam.nodes.size(), verts.size());
                cage = true;
            } else {
                AVER_WARN("[Sandbox] could not load '{}' ({})", beamPath_, err);
            }
        }
        if (!cage) {
            appendBox(verts, indices, 0, 0, 0, 1.0f);
            meshRadius_ = 1.0f; center_ = Vec3{0, 0, 0};
        }
        mesh_ = e.device()->createMesh(verts.data(), static_cast<u32>(verts.size()), indices.data(), static_cast<u32>(indices.size()));
        triPerObject_ = static_cast<u32>(indices.size()) / 3;

        if (cage) {
            objects_.push_back({"Vehicle Cage", {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, {0.85f, 0.36f, 0.22f, 1}, true});
        } else {
            const f32 sp = 3.0f;
            objects_.push_back({"Cube A", {-sp, 0, 0}, {0, 0, 0}, {1, 1, 1}, {0.85f, 0.35f, 0.25f, 1}, true});
            objects_.push_back({"Cube B", {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, {0.35f, 0.65f, 0.85f, 1}, true});
            objects_.push_back({"Cube C", {sp, 0, 0}, {0, 0, 0}, {1, 1, 1}, {0.55f, 0.80f, 0.40f, 1}, true});
            center_ = Vec3{0, 0, 0}; meshRadius_ = sp + 1.0f;
        }
        selected_ = objects_.empty() ? -1 : 0;
        distScale_ = 1.0f; azimuth_ = 0.7f; elevation_ = 0.45f;
    }

    void onUpdate(Engine& e, const Timestep& t) override {
        bool overUI = e.device()->uiWantsMouse();
#if AVER_WITH_IMGUI
        if (e.device()->uiActive()) {
            const ImGuiIO& io = ImGui::GetIO();
            if (!overUI) {
                if (io.MouseDown[1]) { azimuth_ -= io.MouseDelta.x * 0.01f; elevation_ += io.MouseDelta.y * 0.01f; }
                if (io.MouseWheel != 0.0f) distScale_ *= (1.0f - io.MouseWheel * 0.1f);
            }
            const f32 lim = 1.55f;
            elevation_ = elevation_ < -lim ? -lim : (elevation_ > lim ? lim : elevation_);
            distScale_ = distScale_ < 0.15f ? 0.15f : (distScale_ > 8.0f ? 8.0f : distScale_);
            const bool interacting = io.MouseDown[0] || io.MouseDown[1];
            if (autoOrbit_ && !interacting) azimuth_ += t.dt * 0.4f;
        } else
#endif
        { if (autoOrbit_) azimuth_ += t.dt * 0.4f; }

        const f32 dist = meshRadius_ * 3.0f * distScale_ + 4.0f;
        const f32 ch = std::cos(elevation_);
        const Vec3 eye = center_ + Vec3{std::cos(azimuth_) * ch, std::sin(azimuth_) * ch, std::sin(elevation_)} * dist;
        const f32 aspect = viewAspect(e);
        const Mat4 view = Mat4::lookAtLH(eye, center_, Vec3{0, 0, 1});
        const Mat4 proj = Mat4::perspectiveLH(radians(50.0f), aspect, 1.0f, dist * 8.0f + 2000.0f);
        const Mat4 viewProj = view * proj;
        e.device()->setCamera(&viewProj.m[0][0], &eye.x);

        const Vec3 ld = Vec3{0.4f, 0.5f, 0.85f}.getSafeNormal();
        const f32 lc[3] = {1.0f, 0.98f, 0.95f};
        e.device()->setLight(&ld.x, lc, 0.22f);
        e.device()->setClearColor(0.055f, 0.06f, 0.075f, 1.0f);
    }

    void onRender(Engine& e) override {
        handleManipulation(e);

        // Draw all visible objects (selected one brightened).
        u32 visible = 0;
        for (int i = 0; i < static_cast<int>(objects_.size()); ++i) {
            const SceneObject& o = objects_[i];
            if (!o.visible) continue;
            ++visible;
            Transform tr; tr.position = o.pos; tr.rotation = quatFromEulerDeg(o.rotDeg); tr.scale = o.scale;
            Mat4 world = tr.toMatrix();
            f32 col[4] = {o.color[0], o.color[1], o.color[2], 1.0f};
            if (i == selected_) for (int k = 0; k < 3; ++k) col[k] = std::fmin(1.0f, col[k] * 1.35f + 0.12f);
            e.device()->drawMesh(mesh_, &world.m[0][0], col);
        }

        buildUI(e, visible);
        captureCheck(e);
    }

    void onShutdown(Engine&) override { AVER_INFO("[Sandbox] shutdown"); }

private:
    static f32 viewAspect(Engine& e) {
        if (e.window() && e.window()->height())
            return static_cast<f32>(e.window()->width()) / static_cast<f32>(e.window()->height());
        return 1.777f;
    }

    void handleManipulation(Engine& e) {
#if AVER_WITH_IMGUI
        if (!e.device()->uiActive()) return;
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsKeyPressed(ImGuiKey_1)) tool_ = Tool::Select;
        if (ImGui::IsKeyPressed(ImGuiKey_2)) tool_ = Tool::Move;
        if (ImGui::IsKeyPressed(ImGuiKey_3)) tool_ = Tool::Rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_4)) tool_ = Tool::Scale;

        if (io.WantCaptureMouse || !io.MouseDown[0]) return;
        if (selected_ < 0 || selected_ >= static_cast<int>(objects_.size())) return;
        SceneObject& o = objects_[selected_];
        const f32 dx = io.MouseDelta.x, dy = io.MouseDelta.y;
        const f32 k = meshRadius_ * 0.01f + 0.05f;
        switch (tool_) {
            case Tool::Move:   o.pos.x += dx * k; o.pos.y -= dy * k; break;
            case Tool::Rotate: o.rotDeg.z += dx * 0.5f; o.rotDeg.x += dy * 0.5f; break;
            case Tool::Scale:  { f32 s = 1.0f + dx * 0.005f; o.scale = o.scale * (s > 0.05f ? s : 0.05f); } break;
            case Tool::Select: break;
        }
#else
        (void)e;
#endif
    }

    void buildUI(Engine& e, u32 visibleObjects) {
#if AVER_WITH_IMGUI
        if (!e.device()->uiActive()) return;
        const f32 ww = e.window() ? static_cast<f32>(e.window()->width()) : 1440.0f;

        if (ImGui::BeginMainMenuBar()) {
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.20f, 1.0f), "\xC6"); // AE-ish mark
            ImGui::TextUnformatted("Aver Engine");
            ImGui::Separator();
            if (ImGui::BeginMenu("File")) { if (ImGui::MenuItem("Quit")) e.requestExit(); ImGui::EndMenu(); }
            if (ImGui::BeginMenu("View")) { ImGui::MenuItem("Auto-orbit", nullptr, &autoOrbit_); ImGui::EndMenu(); }
            ImGui::EndMainMenuBar();
        }

        ImGui::SetNextWindowPos(ImVec2(8, 28), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(430, 74), ImGuiCond_FirstUseEver);
        ImGui::Begin("Tools");
        ImGui::TextUnformatted("Transform mode:");
        for (int i = 0; i < 4; ++i) {
            if (ImGui::RadioButton(kToolNames[i], static_cast<int>(tool_) == i)) tool_ = static_cast<Tool>(i);
            if (i < 3) ImGui::SameLine();
        }
        ImGui::TextDisabled("Keys 1-4  |  L-drag: manipulate  R-drag: orbit  wheel: zoom");
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(8, 110), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(240, 260), ImGuiCond_FirstUseEver);
        ImGui::Begin("Scene");
        for (int i = 0; i < static_cast<int>(objects_.size()); ++i) {
            if (ImGui::Selectable(objects_[i].name.c_str(), selected_ == i)) selected_ = i;
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(ww - 328, 28), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(320, 340), ImGuiCond_FirstUseEver);
        ImGui::Begin("Inspector");
        if (selected_ >= 0 && selected_ < static_cast<int>(objects_.size())) {
            SceneObject& o = objects_[selected_];
            ImGui::TextUnformatted(o.name.c_str());
            ImGui::Separator();
            ImGui::DragFloat3("Position", &o.pos.x, meshRadius_ * 0.01f + 0.1f);
            ImGui::DragFloat3("Rotation", &o.rotDeg.x, 1.0f);
            ImGui::DragFloat3("Scale", &o.scale.x, 0.01f, 0.02f, 100.0f);
            ImGui::ColorEdit3("Color", o.color);
            ImGui::Checkbox("Visible", &o.visible);
        } else {
            ImGui::TextDisabled("No selection");
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(8, 380), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(240, 150), ImGuiCond_FirstUseEver);
        ImGui::Begin("Stats");
        const f32 dt = e.time().dt;
        ImGui::Text("Backend : %s", rhi::backendName(e.device()->backend()));
        ImGui::Text("Adapter : %s", e.device()->adapterName());
        ImGui::Text("FPS     : %.0f (%.2f ms)", dt > 1e-6f ? 1.0f / dt : 0.0f, dt * 1000.0f);
        ImGui::Text("Objects : %u visible / %zu", visibleObjects, objects_.size());
        ImGui::Text("Tris    : %u", visibleObjects * triPerObject_);
        ImGui::Text("Tool    : %s", kToolNames[static_cast<int>(tool_)]);
        ImGui::End();
#else
        (void)e; (void)visibleObjects;
#endif
    }

    void captureCheck(Engine& e) {
        const u64 frame = e.time().frame;
        u32 ww = 1440, wh = 810;
        if (e.window()) { ww = e.window()->width(); wh = e.window()->height(); }
        // Capture a settled frame (a few in, so ImGui/layout are established).
        const u64 shotFrame = maxFrames_ > 8 ? maxFrames_ - 3 : 4;
        if (frame == shotFrame) e.device()->requestCapture(ww / 2, wh / 2);
        if (frame > shotFrame && !captureLogged_) {
            f32 px[4];
            if (e.device()->getCapture(px)) {
                AVER_INFO("[Sandbox] centre pixel ({:.3f},{:.3f},{:.3f}) => rendered: {}",
                          px[0], px[1], px[2], (px[0] > 0.12f || px[1] > 0.12f) ? "YES" : "no");
            }
            if (!screenshotPath_.empty()) {
                std::vector<u8> img; u32 iw = 0, ih = 0;
                if (e.device()->getFrameImage(img, iw, ih) && iw && ih) {
                    if (stbi_write_png(screenshotPath_.c_str(), static_cast<int>(iw), static_cast<int>(ih), 4,
                                       img.data(), static_cast<int>(iw * 4))) {
                        AVER_INFO("[Sandbox] screenshot saved: {} ({}x{})", screenshotPath_, iw, ih);
                    } else {
                        AVER_WARN("[Sandbox] screenshot write failed: {}", screenshotPath_);
                    }
                }
            }
            captureLogged_ = true;
        }
    }

    u64 maxFrames_; bool headless_; std::string beamPath_; std::string screenshotPath_;
    rhi::MeshHandle mesh_ = 0;
    u32 triPerObject_ = 0;
    std::vector<SceneObject> objects_;
    int selected_ = -1;
    Tool tool_ = Tool::Select;
    bool autoOrbit_ = true;
    Vec3 center_{0, 0, 0};
    f32 meshRadius_ = 1.0f;
    f32 azimuth_ = 0.7f, elevation_ = 0.45f, distScale_ = 1.0f;
    bool captureLogged_ = false;
};

Application* createApplication(int argc, char** argv) {
    u64 frames = 0; bool headless = false; std::string beamPath, screenshotPath;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) headless = true;
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::strtoull(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) screenshotPath = argv[++i];
        else if (argv[i][0] != '-') beamPath = argv[i];
    }
    return new SandboxApp(frames, headless, beamPath, screenshotPath);
}

} // namespace aver
