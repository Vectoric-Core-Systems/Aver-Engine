// The on-device skinning check: one synthetic rig, skinned on both sides, compared number by number.
#include "aver/render/SkinSelfTest.hpp"
#include "aver/anim/Pose.hpp"
#include "aver/core/Log.hpp"

#include <cmath>

namespace aver::render {

namespace {

// Vertices in the synthetic mesh. Deliberately not a multiple of the group size, so the shader's
// out-of-range early-out is exercised rather than assumed.
constexpr u32 kTestVerts = 501;
constexpr u32 kTestBones = 5;

// Positions are centimetres and the rig spans a metre, so a tolerance in absolute cm is the honest
// unit. Anything larger than this is a transposed matrix or a wrong stride, not float noise.
constexpr f32 kPosTolerance = 1e-3f;
constexpr f32 kNrmTolerance = 1e-4f;

// A deterministic sequence, so a failure reproduces exactly rather than "sometimes".
struct Lcg {
    u32 s;
    f32 next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<f32>((s >> 8) & 0xFFFFFF) / 16777216.0f;
    }
};

// Rotation about X, in the engine's row-vector convention: the rows are where the basis goes.
Mat4 rotX(f32 a) {
    const f32 c = std::cos(a), s = std::sin(a);
    Mat4 m;
    m.m[1][1] =  c; m.m[1][2] = s;
    m.m[2][1] = -s; m.m[2][2] = c;
    return m;
}

} // namespace

SkinSelfTest::~SkinSelfTest() { shutdown(); }

bool SkinSelfTest::init(rhi::IDevice& dev) {
    shutdown();
    dev_ = &dev;
    res_ = dev.resources();
    if (!res_ || !pass_.init(dev)) return false;

    // --- the rig. Built as skinning matrices directly rather than posed from an .ocskel: what is
    // under test is the dispatch, and poseToSkinning has its own arithmetic test. Every matrix here
    // has translation, rotation and non-uniform scale, because a shader that reads the matrix
    // transposed still gets a pure rotation right. ---
    skin_.resize(kTestBones);
    Lcg rng{0x5EED1234u};
    for (u32 b = 0; b < kTestBones; ++b) {
        Mat4 m = rotX(rng.next() * 2.0f - 1.0f);
        m.m[0][0] *= 0.8f + rng.next();
        m.m[1][1] *= 0.8f + rng.next();
        m.m[2][2] *= 0.8f + rng.next();
        m.m[3][0] = (rng.next() - 0.5f) * 200.0f;
        m.m[3][1] = (rng.next() - 0.5f) * 200.0f;
        m.m[3][2] = (rng.next() - 0.5f) * 200.0f;
        skin_[b] = m;
    }

    // --- the mesh. ---
    fmt::OcMeshData mesh;
    mesh.positions.resize(static_cast<usize>(kTestVerts) * 3);
    mesh.normals.resize(static_cast<usize>(kTestVerts) * 3);
    mesh.uvs.resize(static_cast<usize>(kTestVerts) * 2, 0.0f);
    mesh.joints.resize(static_cast<usize>(kTestVerts) * 4);
    mesh.weights.resize(static_cast<usize>(kTestVerts) * 4);
    mesh.indices.assign(3, 0);
    mesh.flags = fmt::kOcMeshHasSkin;

    for (u32 v = 0; v < kTestVerts; ++v) {
        mesh.positions[v * 3 + 0] = (rng.next() - 0.5f) * 100.0f;
        mesh.positions[v * 3 + 1] = (rng.next() - 0.5f) * 100.0f;
        mesh.positions[v * 3 + 2] = rng.next() * 180.0f;

        const f32 nx = rng.next() * 2.0f - 1.0f, ny = rng.next() * 2.0f - 1.0f, nz = rng.next() * 2.0f - 1.0f;
        const f32 len = std::sqrt(nx * nx + ny * ny + nz * nz) + 1e-6f;
        mesh.normals[v * 3 + 0] = nx / len;
        mesh.normals[v * 3 + 1] = ny / len;
        mesh.normals[v * 3 + 2] = nz / len;

        f32 w[4];
        f32 sum = 0.0f;
        for (u32 i = 0; i < 4; ++i) { w[i] = rng.next(); sum += w[i]; }
        for (u32 i = 0; i < 4; ++i) {
            mesh.joints[v * 4 + i] = static_cast<u16>((v + i) % kTestBones);
            mesh.weights[v * 4 + i] = w[i] / sum;
        }

        // Two vertices in ten exercise the branches that a uniformly-rigged mesh never reaches:
        // an influence with zero weight, an index past the bound rig, and a vertex with no
        // surviving influence at all. Both sides must fall back the same way.
        if (v % 10 == 3) mesh.weights[v * 4 + 2] = 0.0f;
        if (v % 10 == 7) mesh.joints[v * 4 + 1] = static_cast<u16>(kTestBones + 4);
        if (v % 97 == 11) for (u32 i = 0; i < 4; ++i) mesh.weights[v * 4 + i] = 0.0f;
    }

    anim::skinVertices(skin_, mesh.positions, mesh.normals, mesh.joints, mesh.weights,
                       cpuPositions_, cpuNormals_);
    if (cpuPositions_.size() != static_cast<usize>(kTestVerts) * 3) {
        AVER_ERROR("[Skin] self-test: the CPU reference rejected its own inputs");
        shutdown();
        return false;
    }

    if (!pass_.createMesh(mesh, mesh_)) { shutdown(); return false; }

    rhi::BufferDesc rb;
    rb.bytes = static_cast<u64>(kTestVerts) * kSkinVertexStride;
    rb.kind  = rhi::BufferKind::Readback;
    rb.debugName = "skin self-test readback";
    readback_ = res_->createBuffer(rb);
    if (!readback_) { shutdown(); return false; }

    return true;
}

void SkinSelfTest::shutdown() {
    if (res_ && readback_) res_->destroyBuffer(readback_);
    readback_ = 0;
    pass_.destroyMesh(mesh_);
    pass_.shutdown();
    skin_.clear();
    cpuPositions_.clear();
    cpuNormals_.clear();
    stage_ = 0;
    dev_ = nullptr;
    res_ = nullptr;
}

void SkinSelfTest::prePass(rhi::IRenderContext& ctx) {
    if (stage_ == 0) {
        if (!mesh_.valid() || !readback_) { stage_ = 2; return; }

        pass_.dispatch(ctx, mesh_, skin_.data(), static_cast<u32>(skin_.size()));

        // dispatch left it in VertexBuffer, which is where a raster consumer wants it. The copy
        // needs CopySource, and the frame must END in Common because a buffer's state does not
        // survive the command list.
        skinTransition(ctx, mesh_, rhi::ResourceState::CopySource);
        ctx.copyBuffer(readback_, mesh_.out, static_cast<u64>(mesh_.vertexCount) * kSkinVertexStride);
        skinTransition(ctx, mesh_, rhi::ResourceState::Common);

        stage_ = 1;
        return;
    }
    if (stage_ == 1) {
        // The copy was recorded LAST frame, and that frame has been submitted. waitIdle is what
        // makes the readback mean anything: readBuffer synchronises nothing by contract.
        res_->waitIdle();
        compare();
        stage_ = 2;
    }
}

void SkinSelfTest::compare() {
    std::vector<f32> gpu(static_cast<usize>(mesh_.vertexCount) * 6);
    if (!res_->readBuffer(readback_, gpu.data(), gpu.size() * sizeof(f32))) {
        AVER_ERROR("[Skin] self-test: readback failed");
        return;
    }

    u32 worstVert = 0;
    for (u32 v = 0; v < mesh_.vertexCount; ++v) {
        for (u32 c = 0; c < 3; ++c) {
            const f32 dp = std::fabs(gpu[v * 6 + c]     - cpuPositions_[v * 3 + c]);
            const f32 dn = std::fabs(gpu[v * 6 + 3 + c] - cpuNormals_[v * 3 + c]);
            if (dp > worstPos_) { worstPos_ = dp; worstVert = v; }
            if (dn > worstNrm_) worstNrm_ = dn;
        }
    }

    passed_ = worstPos_ <= kPosTolerance && worstNrm_ <= kNrmTolerance;
    if (passed_) {
        AVER_INFO("[Skin] self-test PASS: {} vertices, {} bones, worst position {:.6f} cm, worst normal {:.6f}",
                  mesh_.vertexCount, skin_.size(), worstPos_, worstNrm_);
    } else {
        AVER_ERROR("[Skin] self-test FAIL: worst position {:.6f} cm at vertex {} (tolerance {}), worst normal {:.6f} (tolerance {})",
                   worstPos_, worstVert, kPosTolerance, worstNrm_, kNrmTolerance);
        AVER_ERROR("[Skin]   vertex {}: gpu ({:.4f}, {:.4f}, {:.4f})  cpu ({:.4f}, {:.4f}, {:.4f})",
                   worstVert, gpu[worstVert * 6 + 0], gpu[worstVert * 6 + 1], gpu[worstVert * 6 + 2],
                   cpuPositions_[worstVert * 3 + 0], cpuPositions_[worstVert * 3 + 1],
                   cpuPositions_[worstVert * 3 + 2]);
    }
}

} // namespace aver::render
