#include "aver/runtime/EntryPoint.hpp"
#include "aver/platform/Window.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcBeam.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace aver {

// Append an axis-aligned box (24 verts, per-face normals; cull is off so winding is
// irrelevant) centred at (cx,cy,cz) with half-size h.
static void appendBox(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx,
                      f32 cx, f32 cy, f32 cz, f32 h) {
    const f32 p[8][3] = {
        {-h,-h,-h}, {h,-h,-h}, {h,h,-h}, {-h,h,-h},
        {-h,-h, h}, {h,-h, h}, {h,h, h}, {-h,h, h}};
    struct Face { f32 n[3]; int c[4]; };
    const Face faces[6] = {
        {{ 1, 0, 0}, {1, 2, 6, 5}},
        {{-1, 0, 0}, {0, 4, 7, 3}},
        {{ 0, 1, 0}, {3, 7, 6, 2}},
        {{ 0,-1, 0}, {0, 1, 5, 4}},
        {{ 0, 0, 1}, {4, 5, 6, 7}},
        {{ 0, 0,-1}, {0, 3, 2, 1}}};
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

class SandboxApp final : public Application {
public:
    SandboxApp(u64 maxFrames, bool headless, std::string beamPath)
        : maxFrames_(maxFrames), headless_(headless), beamPath_(std::move(beamPath)) {}

    BootConfig config() const override {
        BootConfig c;
        c.windowTitle = "Aver Engine \xE2\x80\x94 Sandbox";
        c.windowWidth = 1280; c.windowHeight = 720;
        c.maxFrames = maxFrames_; c.headless = headless_;
        return c;
    }

    void onInit(Engine& e) override {
        AVER_INFO("[Sandbox] init on RHI backend={} adapter='{}'",
                  rhi::backendName(e.device()->backend()), e.device()->adapterName());

        const f32 in[4] = {0.10f, 0.20f, 0.30f, 1.0f};
        f32 out[4] = {};
        if (e.device()->selfTest(in, out)) {
            const bool ok = std::fabs(out[0] - in[0]) < 0.01f && std::fabs(out[2] - in[2]) < 0.01f;
            AVER_INFO("[Sandbox] GPU self-test read ({:.3f},{:.3f},{:.3f}) => {}", out[0], out[1], out[2], ok ? "PASSED" : "MISMATCH");
        }

        std::vector<rhi::MeshVertex> verts;
        std::vector<u32> indices;
        bounds_ = AABB{};

        bool built = false;
        if (!beamPath_.empty()) {
            fmt::OcBeamData beam;
            std::string err;
            if (fmt::loadOcbeam(beamPath_, beam, &err) && !beam.nodes.empty()) {
                // Bounds of the cage.
                bounds_.min = bounds_.max = Vec3{beam.nodes[0].x, beam.nodes[0].y, beam.nodes[0].z};
                for (const auto& n : beam.nodes) bounds_.expand(Vec3{n.x, n.y, n.z});
                const Vec3 ext = bounds_.extent();
                const f32 span = (ext.x + ext.y + ext.z);
                const f32 dot = span * 0.004f + 1.0f; // node marker half-size
                for (const auto& n : beam.nodes) appendBox(verts, indices, n.x, n.y, n.z, dot);
                AVER_INFO("[Sandbox] rendering cage '{}' as {} node markers ({} verts)",
                          beamPath_, beam.nodes.size(), verts.size());
                built = true;
            } else {
                AVER_WARN("[Sandbox] could not load '{}' ({}); rendering a cube instead", beamPath_, err);
            }
        }
        if (!built) {
            appendBox(verts, indices, 0, 0, 0, 1.0f);
            bounds_.min = Vec3{-1, -1, -1}; bounds_.max = Vec3{1, 1, 1};
            AVER_INFO("[Sandbox] rendering a shaded cube ({} verts)", verts.size());
        }

        mesh_ = e.device()->createMesh(verts.data(), static_cast<u32>(verts.size()),
                                       indices.data(), static_cast<u32>(indices.size()));
        indexCount_ = static_cast<u32>(indices.size());
    }

    // onUpdate = per-frame state that beginFrame() consumes (clear colour, camera, light).
    // Runs BEFORE beginFrame, so it must NOT record draws.
    void onUpdate(Engine& e, const Timestep& t) override {
        const f32 b = 0.06f + 0.03f * (0.5f + 0.5f * std::sin(t.total));
        e.device()->setClearColor(0.03f, 0.04f, b, 1.0f);

        const Vec3 lightDir = Vec3{0.4f, 0.5f, 0.85f}.getSafeNormal();
        const f32 lc[3] = {1.0f, 0.98f, 0.95f};
        e.device()->setLight(&lightDir.x, lc, 0.22f);

        // Orbit the camera around the object's bounds.
        const Vec3 center = bounds_.center();
        const Vec3 ext = bounds_.extent();
        f32 radius = ext.x; radius = ext.y > radius ? ext.y : radius; radius = ext.z > radius ? ext.z : radius;
        radius = radius * 2.6f + 4.0f;

        const f32 a = t.total * 0.6f;
        const Vec3 eye = center + Vec3{std::cos(a) * radius, std::sin(a) * radius, radius * 0.55f};
        const f32 aspect = viewAspect(e);

        const Mat4 view = Mat4::lookAtLH(eye, center, Vec3{0, 0, 1});
        const Mat4 proj = Mat4::perspectiveLH(radians(50.0f), aspect, 1.0f, radius * 8.0f + 1000.0f);
        const Mat4 viewProj = view * proj;
        e.device()->setCamera(&viewProj.m[0][0], &eye.x);

        if (t.frame <= 3 || (t.frame % 120) == 0) {
            AVER_INFO("[Sandbox] frame {} dt={:.4f}s", t.frame, t.dt);
        }
    }

    // onRender = record draws (runs AFTER beginFrame, into the open command list).
    void onRender(Engine& e) override {
        Mat4 world = Mat4::identity();
        const f32 objColor[4] = {0.88f, 0.36f, 0.22f, 1.0f};
        e.device()->drawMesh(mesh_, &world.m[0][0], objColor);

        // Verify an object actually rasterized at screen centre.
        const u64 frame = e.time().frame;
        u32 ww = 1280, wh = 720;
        if (e.window()) { ww = e.window()->width(); wh = e.window()->height(); }
        if (frame == 3) e.device()->requestCapture(ww / 2, wh / 2);
        if (frame >= 4 && !captureLogged_) {
            f32 px[4];
            if (e.device()->getCapture(px)) {
                const bool objectAtCenter = px[0] > px[2] && px[0] > 0.15f; // reddish object vs blue bg
                AVER_INFO("[Sandbox] centre pixel ({:.3f},{:.3f},{:.3f}) => object rendered: {}",
                          px[0], px[1], px[2], objectAtCenter ? "YES" : "no");
                captureLogged_ = true;
            }
        }
    }

    void onShutdown(Engine&) override { AVER_INFO("[Sandbox] shutdown"); }

private:
    static f32 viewAspect(Engine& e) {
        if (e.window() && e.window()->height()) {
            return static_cast<f32>(e.window()->width()) / static_cast<f32>(e.window()->height());
        }
        return 1.777f;
    }

private:
    u64 maxFrames_;
    bool headless_;
    std::string beamPath_;
    rhi::MeshHandle mesh_ = 0;
    u32 indexCount_ = 0;
    AABB bounds_;
    bool captureLogged_ = false;
};

Application* createApplication(int argc, char** argv) {
    u64 frames = 0;
    bool headless = false;
    std::string beamPath;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) headless = true;
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::strtoull(argv[++i], nullptr, 10);
        else if (argv[i][0] != '-') beamPath = argv[i]; // positional: an .ocbeam to render
    }
    return new SandboxApp(frames, headless, beamPath);
}

} // namespace aver
