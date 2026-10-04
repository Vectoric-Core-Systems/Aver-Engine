// The skinned-draw check: one box, one ground plane, three experiments, nine pixels.
#include "SkinDrawTest.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <vector>

namespace aver::editor {

namespace {

// Half-extent of the box, in centimetres. Large enough to cover the viewport centre from the
// editor's default camera at (700, 700, 450) looking at the origin, so the centre probe lands ON it
// without the test having to know anything about the projection.
constexpr f32 kHalf = 220.0f;

// The ground: a large flat quad just under the box, which exists only to have a shadow fall on it.
constexpr f32 kGroundZ = -230.0f;
constexpr f32 kGroundHalf = 1600.0f;

// How far the "away" pose takes the box: straight down, well below the ground, so it neither draws
// nor casts. Far enough to be unambiguous rather than merely moved.
constexpr f32 kAway = 100000.0f;

// What counts as a changed pixel. Per channel, and generous enough that no dither or temporal
// jitter reaches it in a headless run with a static camera and a static sun.
constexpr f32 kMinDelta = 0.05f;

// A box as 24 vertices, four per face, so each face carries its own flat normal. Every vertex is
// bound wholly to bone 0: the point is to move the whole box by a bone, not to test blending, which
// the numeric self-test already covers vertex by vertex.
void buildBox(fmt::OcMeshData& m, f32 h) {
    const f32 n[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    const f32 c[6][4][3] = {
        {{ h,-h,-h},{ h, h,-h},{ h, h, h},{ h,-h, h}},
        {{-h, h,-h},{-h,-h,-h},{-h,-h, h},{-h, h, h}},
        {{ h, h,-h},{-h, h,-h},{-h, h, h},{ h, h, h}},
        {{-h,-h,-h},{ h,-h,-h},{ h,-h, h},{-h,-h, h}},
        {{-h,-h, h},{ h,-h, h},{ h, h, h},{-h, h, h}},
        {{-h, h,-h},{ h, h,-h},{ h,-h,-h},{-h,-h,-h}},
    };

    m = {};
    for (u32 f = 0; f < 6; ++f) {
        const u32 base = f * 4;
        for (u32 v = 0; v < 4; ++v) {
            m.positions.insert(m.positions.end(), {c[f][v][0], c[f][v][1], c[f][v][2]});
            m.normals.insert(m.normals.end(), {n[f][0], n[f][1], n[f][2]});
            m.uvs.insert(m.uvs.end(), {static_cast<f32>(v & 1), static_cast<f32>((v >> 1) & 1)});
            m.joints.insert(m.joints.end(), {0, 0, 0, 0});
            m.weights.insert(m.weights.end(), {1.0f, 0.0f, 0.0f, 0.0f});
        }
        m.indices.insert(m.indices.end(),
                         {base + 0, base + 1, base + 2, base + 0, base + 2, base + 3});
    }
    m.flags = fmt::kOcMeshHasSkin;
    m.computeBounds();
}

// Uploads an OcMeshData as an ordinary static mesh.
rhi::MeshHandle upload(rhi::IDevice& dev, const fmt::OcMeshData& m) {
    std::vector<rhi::MeshVertex> verts(m.vertexCount());
    for (u32 v = 0; v < m.vertexCount(); ++v) {
        rhi::MeshVertex& o = verts[v];
        o.px = m.positions[v * 3 + 0]; o.py = m.positions[v * 3 + 1]; o.pz = m.positions[v * 3 + 2];
        o.nx = m.normals[v * 3 + 0];   o.ny = m.normals[v * 3 + 1];   o.nz = m.normals[v * 3 + 2];
        o.u  = m.uvs[v * 2 + 0];       o.v  = m.uvs[v * 2 + 1];
    }
    return dev.createMesh(verts.data(), static_cast<u32>(verts.size()),
                          m.indices.data(), static_cast<u32>(m.indices.size()));
}

} // namespace

SkinDrawTest::~SkinDrawTest() { shutdown(); }

bool SkinDrawTest::init(rhi::IDevice& dev) {
    shutdown();

    if (!pass_.init(dev)) return false;

    fmt::OcMeshData box;
    buildBox(box, kHalf);

    // The source mesh: an ordinary upload, exactly as any static mesh is made. Its vertices are
    // what seed the skin target, so a frame drawn before the first dispatch shows the bind pose.
    rest_ = upload(dev, box);
    if (!rest_) { AVER_ERROR("[Skin] draw test: the source mesh would not upload"); shutdown(); return false; }

    skinned_ = dev.createSkinTargetMesh(rest_, &vertices_);
    if (!skinned_ || !vertices_) {
        AVER_ERROR("[Skin] draw test: this device has no skin-target mesh");
        shutdown();
        return false;
    }

    // One bone, and the skin target IS the mesh's vertex buffer -- which is the whole point: the
    // compute pass writes the buffer the input assembler reads, with nothing in between.
    if (!pass_.createMesh(box, 1, gpu_, vertices_)) { shutdown(); return false; }

    // The ground. A quad rather than a plane primitive because the ray tracer needs triangles, and
    // STATIC because a shadow receiver that moved for its own reasons would prove nothing.
    {
        fmt::OcMeshData g;
        const f32 h = kGroundHalf;
        const f32 p[4][3] = {{-h,-h,kGroundZ},{ h,-h,kGroundZ},{ h, h,kGroundZ},{-h, h,kGroundZ}};
        for (u32 v = 0; v < 4; ++v) {
            g.positions.insert(g.positions.end(), {p[v][0], p[v][1], p[v][2]});
            g.normals.insert(g.normals.end(), {0.0f, 0.0f, 1.0f});
            g.uvs.insert(g.uvs.end(), {static_cast<f32>(v & 1), static_cast<f32>((v >> 1) & 1)});
        }
        g.indices = {0, 1, 2, 0, 2, 3};
        g.computeBounds();
        ground_ = upload(dev, g);
        if (!ground_) { AVER_ERROR("[Skin] draw test: the ground would not upload"); shutdown(); return false; }
    }

    // Probe 0 is the box. The rest sweep the lower half of the viewport, which is where the ground
    // is from the default camera.
    probes_[0].u = 0.5f; probes_[0].v = 0.5f;
    const f32 gu[kProbes - 1] = {0.30f, 0.40f, 0.50f, 0.60f, 0.70f, 0.42f, 0.58f, 0.50f};
    const f32 gv[kProbes - 1] = {0.78f, 0.82f, 0.86f, 0.82f, 0.78f, 0.72f, 0.72f, 0.94f};
    for (u32 i = 1; i < kProbes; ++i) { probes_[i].u = gu[i - 1]; probes_[i].v = gv[i - 1]; }

    poseNear_[0] = Mat4::identity();
    poseAway_[0] = Mat4::identity();
    poseAway_[0].m[3][2] = -kAway;

    AVER_INFO("[Skin] draw test armed: {} vertices skinned into the mesh's own vertex buffer, "
              "{} probes over {} experiments", gpu_.vertexCount, kProbes, u32(kPhaseCount));
    return true;
}

void SkinDrawTest::shutdown() {
    pass_.destroyMesh(gpu_);
    pass_.shutdown();
    // The meshes are not released although IDevice::destroyMesh exists now: this is a short test
    // run, and a skin target must be destroyed before its source.
    rest_ = skinned_ = ground_ = 0;
    vertices_ = 0;
    step_ = 0;
    phase_ = kPhaseNearRtOn;
    done_ = false;
    for (Probe& p : probes_) p = {};
}

void SkinDrawTest::prePass(rhi::IRenderContext& ctx) {
    if (!gpu_.valid()) return;
    // The box is near only in the first experiment; the other two hold it a kilometre away so that
    // ray tracing is the single thing that varies between them.
    pass_.dispatch(ctx, gpu_, phase_ == kPhaseNearRtOn ? poseNear_ : poseAway_, 1);
}

void SkinDrawTest::overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) {
    (void)width; (void)height;
    if (gpu_.valid()) skinTransition(ctx, gpu_, rhi::ResourceState::Common);
}

void SkinDrawTest::tick(rhi::IDevice& dev, f32 vpX, f32 vpY, f32 vpW, f32 vpH) {
    if (done_) return;

    // Frames to settle before sampling. The renderer replays LAST frame's draw list into its shadow
    // and voxelise passes, and toggling ray tracing rebuilds pipelines, so neither a pose change nor
    // a settings change is fully on screen the frame it happens.
    constexpr u32 kSettle = 8;
    if (step_ < kSettle) { ++step_; return; }

    // Two frames per probe: one to request the capture and one to read it. The read must be a LATER
    // frame -- a capture requested this frame has not resolved.
    const u32 t = step_ - kSettle;
    const u32 slot = t / 2;

    if (slot >= kProbes) {
        if (phase_ + 1 < kPhaseCount) { ++phase_; step_ = 0; return; }   // same probes, next experiment
        report();
        done_ = true;
        return;
    }

    Probe& p = probes_[slot];
    if ((t % 2) == 0) {
        dev.requestCapture(static_cast<u32>(vpX + vpW * p.u), static_cast<u32>(vpY + vpH * p.v));
    } else {
        f32 c[4];
        if (dev.getCapture(c)) {
            for (u32 i = 0; i < 4; ++i) p.c[phase_][i] = c[i];
            p.have[phase_] = true;
        }
    }
    ++step_;
}

void SkinDrawTest::report() {
    const auto raw = [](f32 c) { return static_cast<int>(c * 255.0f + 0.5f); };
    // Worst per-channel difference between two phases of one probe.
    const auto diff = [](const Probe& p, u32 x, u32 y) {
        f32 w = 0.0f;
        for (u32 i = 0; i < 3; ++i) w = std::fmax(w, std::fabs(p.c[x][i] - p.c[y][i]));
        return w;
    };
    // How much DARKER phase y is than phase x. Signed on purpose: a shadow APPEARING is the only
    // thing being asked about, and a pixel getting brighter is not evidence of one.
    const auto darkening = [](const Probe& p, u32 x, u32 y) {
        f32 w = 0.0f;
        for (u32 i = 0; i < 3; ++i) w = std::fmax(w, p.c[x][i] - p.c[y][i]);
        return w;
    };

    // ---- 1. RASTER: the box's own pixel followed the pose ----
    const Probe& box = probes_[0];
    if (!box.have[kPhaseNearRtOn] || !box.have[kPhaseAwayRtOn]) {
        AVER_ERROR("[Skin] draw test INCONCLUSIVE: the box probe did not read back");
        return;
    }
    const f32 boxDelta = diff(box, kPhaseNearRtOn, kPhaseAwayRtOn);
    AVER_INFO("[Skin] draw test: box pixel  near raw ({},{},{})  away raw ({},{},{})  delta {:.4f}",
              raw(box.c[kPhaseNearRtOn][0]), raw(box.c[kPhaseNearRtOn][1]), raw(box.c[kPhaseNearRtOn][2]),
              raw(box.c[kPhaseAwayRtOn][0]), raw(box.c[kPhaseAwayRtOn][1]), raw(box.c[kPhaseAwayRtOn][2]),
              boxDelta);

    const bool rasterOk = boxDelta > kMinDelta;
    if (rasterOk)
        AVER_INFO("[Skin] draw test RASTER PASS: the same pixel changed when only the POSE changed, "
                  "so the rasteriser read the skinned vertex buffer");
    else
        AVER_ERROR("[Skin] draw test RASTER FAIL: the pixel did not move (delta {:.4f} <= {}). The rest "
                   "vertices are identical in both frames, so an unchanged pixel means the draw path "
                   "never read the posed buffer -- a character frozen in bind pose",
                   boxDelta, kMinDelta);

    // ---- 2. ACCELERATION STRUCTURE: with the box gone, turning ray tracing on must not shadow ----
    if (!rtAvailable_) {
        AVER_WARN("[Skin] draw test: ray tracing is unavailable here, so the acceleration-structure "
                  "half DID NOT RUN. The raster result above stands alone and says nothing about "
                  "whether a skinned mesh's bottom-level structure follows its vertices.");
        // A HALF THAT COULD NOT RUN IS NOT A FAILURE. The machine has no ray tracing; reporting a
        // failure here would tell a script the skinned draw path is broken when the test never
        // looked at it. The raster half is what was actually measured, so it is what is reported.
        passed_ = rasterOk;
        return;
    }

    u32 checked = 0, shadowed = 0, worstAt = 0;
    f32 worstDark = 0.0f;
    for (u32 i = 1; i < kProbes; ++i) {
        const Probe& p = probes_[i];
        if (!p.have[kPhaseAwayRtOff] || !p.have[kPhaseAwayRtOn]) continue;
        ++checked;
        const f32 d = darkening(p, kPhaseAwayRtOff, kPhaseAwayRtOn);
        if (d > worstDark) { worstDark = d; worstAt = i; }
        if (d > kMinDelta) ++shadowed;
        AVER_INFO("[Skin]   ground probe {} (u {:.2f} v {:.2f}): rt off ({},{},{})  rt on ({},{},{})  "
                  "darkening {:.4f}",
                  i, p.u, p.v,
                  raw(p.c[kPhaseAwayRtOff][0]), raw(p.c[kPhaseAwayRtOff][1]), raw(p.c[kPhaseAwayRtOff][2]),
                  raw(p.c[kPhaseAwayRtOn][0]), raw(p.c[kPhaseAwayRtOn][1]), raw(p.c[kPhaseAwayRtOn][2]), d);
    }

    if (checked == 0) {
        AVER_ERROR("[Skin] draw test STRUCTURE INCONCLUSIVE: no ground probe read back in both halves");
        // INCONCLUSIVE IS NOT A PASS. Nothing was measured, so there is nothing to report green.
        passed_ = false;
        return;
    }
    if (shadowed == 0) {
        AVER_INFO("[Skin] draw test STRUCTURE PASS: with the box a kilometre away, enabling ray "
                  "tracing darkened no ground pixel by more than {} (worst {:.4f} at probe {}), so "
                  "the bottom-level structure followed the skinned vertices instead of keeping the "
                  "silhouette it was built with",
                  kMinDelta, worstDark, worstAt);
        // BOTH HALVES, not just this one: a structure pass beside a raster failure is still a
        // failing run, and reporting it green would hide exactly the case this test exists for.
        passed_ = rasterOk;
        return;
    }
    AVER_ERROR("[Skin] draw test STRUCTURE FAIL: {} of {} ground pixels went DARKER when ray tracing "
               "was enabled, worst {:.4f} at probe {}. The box is a kilometre below the ground and "
               "casts no cascade shadow there, so a ray-traced one can only come from a bottom-level "
               "structure still describing the vertices it was first built from -- a character whose "
               "shadow stays where it started",
               shadowed, checked, worstDark, worstAt);
}

} // namespace aver::editor
