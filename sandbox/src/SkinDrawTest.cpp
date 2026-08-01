// The skinned-draw check: one box, two poses, one pixel.
#include "SkinDrawTest.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <vector>

namespace aver::editor {

namespace {

// Half-extent of the box, in centimetres. Large enough to cover the viewport centre from the
// editor's default camera at (700, 700, 450) looking at the origin, so the probe lands ON it
// without the test having to know anything about the projection.
constexpr f32 kHalf = 220.0f;

// How far pose B takes the box. Far enough to be unambiguously out of frame rather than merely
// moved: a partial move could leave the probe on the box by luck and read the same colour.
constexpr f32 kAway = 100000.0f;

// The probe must be able to tell "box" from "not box". Colour is the app's, so what is asserted is
// only that the two readings DIFFER by more than this per channel -- which no lighting change can
// manufacture in a headless run with a static camera and a static sun.
constexpr f32 kMinDelta = 0.05f;

// A unit box as 24 vertices, four per face, so each face carries its own flat normal. Every vertex
// is bound wholly to bone 0: the point is to move the whole box by a bone, not to test blending,
// which the numeric self-test already covers vertex by vertex.
void buildBox(fmt::OcMeshData& m) {
    const f32 h = kHalf;
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

} // namespace

SkinDrawTest::~SkinDrawTest() { shutdown(); }

bool SkinDrawTest::init(rhi::IDevice& dev) {
    shutdown();
    dev_ = &dev;
    if (!pass_.init(dev)) return false;

    fmt::OcMeshData box;
    buildBox(box);

    // The source mesh: an ordinary upload, exactly as any static mesh is made. Its vertices are
    // what seed the skin target, so a frame drawn before the first dispatch shows the bind pose.
    std::vector<rhi::MeshVertex> verts(box.vertexCount());
    for (u32 v = 0; v < box.vertexCount(); ++v) {
        rhi::MeshVertex& o = verts[v];
        o.px = box.positions[v * 3 + 0]; o.py = box.positions[v * 3 + 1]; o.pz = box.positions[v * 3 + 2];
        o.nx = box.normals[v * 3 + 0];   o.ny = box.normals[v * 3 + 1];   o.nz = box.normals[v * 3 + 2];
        o.u  = box.uvs[v * 2 + 0];       o.v  = box.uvs[v * 2 + 1];
    }
    rest_ = dev.createMesh(verts.data(), static_cast<u32>(verts.size()),
                           box.indices.data(), static_cast<u32>(box.indices.size()));
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

    poseA_[0] = Mat4::identity();
    poseB_[0] = Mat4::identity();
    poseB_[0].m[3][2] = -kAway;   // straight down, out of frame

    AVER_INFO("[Skin] draw test armed: {} vertices skinned into the mesh's own vertex buffer",
              gpu_.vertexCount);
    return true;
}

void SkinDrawTest::shutdown() {
    pass_.destroyMesh(gpu_);
    pass_.shutdown();
    // rest_ and skinned_ are not released: the RHI has no destroyMesh, which is a known and
    // recorded gap. Two meshes for the life of a test run is not what makes that worth fixing.
    rest_ = skinned_ = 0;
    vertices_ = 0;
    dev_ = nullptr;
    stage_ = 0;
    haveA_ = haveB_ = false;
    poseIsB_ = false;
}

void SkinDrawTest::prePass(rhi::IRenderContext& ctx) {
    if (!gpu_.valid()) return;
    const Mat4* pose = poseIsB_ ? poseB_ : poseA_;
    pass_.dispatch(ctx, gpu_, pose, 1);
}

void SkinDrawTest::overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) {
    (void)width; (void)height;
    if (gpu_.valid()) skinTransition(ctx, gpu_, rhi::ResourceState::Common);
}

void SkinDrawTest::tick(rhi::IDevice& dev, u32 px, u32 py) {
    if (stage_ >= kDone) return;

    switch (stage_) {
        case kGrabA: dev.requestCapture(px, py); break;
        case kReadA: haveA_ = dev.getCapture(a_); break;
        case kPoseBAt: poseIsB_ = true; break;
        case kGrabB: dev.requestCapture(px, py); break;
        case kReadB:
            haveB_ = dev.getCapture(b_);
            report();
            break;
        default: break;
    }
    ++stage_;
}

void SkinDrawTest::report() {
    if (!haveA_ || !haveB_) {
        AVER_ERROR("[Skin] draw test INCONCLUSIVE: the capture did not read back");
        return;
    }

    f32 worst = 0.0f;
    for (u32 i = 0; i < 3; ++i) worst = std::fmax(worst, std::fabs(a_[i] - b_[i]));

    const auto raw = [](f32 c) { return static_cast<int>(c * 255.0f + 0.5f); };
    AVER_INFO("[Skin] draw test: pose A raw ({},{},{})  pose B raw ({},{},{})  worst channel delta {:.4f}",
              raw(a_[0]), raw(a_[1]), raw(a_[2]), raw(b_[0]), raw(b_[1]), raw(b_[2]), worst);

    if (worst > kMinDelta) {
        AVER_INFO("[Skin] draw test PASS: the same pixel changed when only the POSE changed, so the "
                  "rasteriser read the skinned vertex buffer");
        return;
    }
    AVER_ERROR("[Skin] draw test FAIL: the pixel did not move (delta {:.4f} <= {}). The rest vertices "
               "are identical in both frames, so an unchanged pixel means the draw path never read "
               "the posed buffer -- a character frozen in bind pose",
               worst, kMinDelta);
}

} // namespace aver::editor
