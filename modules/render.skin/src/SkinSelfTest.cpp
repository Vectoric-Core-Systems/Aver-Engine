// The on-device skinning check: three synthetic rigs, skinned on both sides, compared number by number.
#include "aver/render/SkinSelfTest.hpp"
#include "aver/anim/Pose.hpp"
#include "aver/core/Log.hpp"

#include <cmath>

namespace aver::render {

namespace {

// Vertices per instance. Deliberately not a multiple of the group size, so the shader's
// out-of-range early-out is exercised rather than assumed.
constexpr u32 kTestVerts = 501;

// Positions are centimetres and each rig spans a metre or two, so a tolerance in absolute cm is the
// honest unit. Anything larger than this is a transposed matrix or a wrong stride, not float noise.
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

// Builds one instance: its rig, its mesh, its CPU answer and its GPU residency. `index` varies the
// seed and the bone count so no two instances can produce the same numbers -- which is what makes a
// crossed dispatch show up as a wrong answer rather than a coincidentally right one.
bool SkinSelfTest::buildCase(u32 index, u32 bones, Case& c) {
    Lcg rng{0x5EED1234u + index * 0x9E3779B9u};

    // Every matrix carries translation, rotation AND non-uniform scale, because a shader that reads
    // the matrix transposed still gets a pure rotation right.
    c.skin.resize(bones);
    for (u32 b = 0; b < bones; ++b) {
        Mat4 m = rotX(rng.next() * 2.0f - 1.0f);
        m.m[0][0] *= 0.8f + rng.next();
        m.m[1][1] *= 0.8f + rng.next();
        m.m[2][2] *= 0.8f + rng.next();
        m.m[3][0] = (rng.next() - 0.5f) * 200.0f;
        m.m[3][1] = (rng.next() - 0.5f) * 200.0f;
        m.m[3][2] = (rng.next() - 0.5f) * 200.0f;
        c.skin[b] = m;
    }

    fmt::OcMeshData mesh;
    mesh.positions.resize(static_cast<usize>(kTestVerts) * 3);
    mesh.normals.resize(static_cast<usize>(kTestVerts) * 3);
    mesh.uvs.resize(static_cast<usize>(kTestVerts) * 2);
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

        // NON-ZERO AND NON-CONSTANT, per vertex and per instance. The uv is the instrument that
        // proves C++ and DXC agree about the element stride, and a zero fill or a constant would
        // let a shifted read pass by accident.
        mesh.uvs[v * 2 + 0] = static_cast<f32>(v) / static_cast<f32>(kTestVerts) + static_cast<f32>(index);
        mesh.uvs[v * 2 + 1] = std::fmod(static_cast<f32>(v) * 0.6180339887f, 1.0f) + 1.0f;

        f32 w[4];
        f32 sum = 0.0f;
        for (u32 i = 0; i < 4; ++i) { w[i] = rng.next(); sum += w[i]; }
        for (u32 i = 0; i < 4; ++i) {
            mesh.joints[v * 4 + i] = static_cast<u16>((v + i) % bones);
            mesh.weights[v * 4 + i] = w[i] / sum;
        }

        // Two vertices in ten exercise the branches that a uniformly-rigged mesh never reaches:
        // an influence with zero weight, an index past the bound rig, and a vertex with no
        // surviving influence at all. Both sides must fall back the same way.
        if (v % 10 == 3) mesh.weights[v * 4 + 2] = 0.0f;
        if (v % 10 == 7) mesh.joints[v * 4 + 1] = static_cast<u16>(bones + 4);
        if (v % 97 == 11) for (u32 i = 0; i < 4; ++i) mesh.weights[v * 4 + i] = 0.0f;
    }

    anim::skinVertices(c.skin, mesh.positions, mesh.normals, mesh.joints, mesh.weights,
                       c.cpuPositions, c.cpuNormals);
    if (c.cpuPositions.size() != static_cast<usize>(kTestVerts) * 3) {
        AVER_ERROR("[Skin] self-test: the CPU reference rejected its own inputs");
        return false;
    }
    c.uvs = mesh.uvs;
    c.vertexCount = kTestVerts;

    if (!pass_.createMesh(mesh, bones, c.gpu)) return false;

    rhi::BufferDesc rb;
    rb.bytes = static_cast<u64>(kTestVerts) * kSkinVertexStride;
    rb.kind  = rhi::BufferKind::Readback;
    rb.debugName = "skin self-test readback";
    c.readback = res_->createBuffer(rb);
    return c.readback != 0;
}

bool SkinSelfTest::init(rhi::IDevice& dev) {
    shutdown();
    dev_ = &dev;
    res_ = dev.resources();
    if (!res_ || !pass_.init(dev)) return false;

    // Different bone counts as well as different seeds: an instance that read another's bone buffer
    // would then also be reading the wrong NUMBER of bones, which the out-of-range fallback turns
    // into a visibly different answer.
    const u32 boneCounts[kCases] = {5, 9, 7};
    for (u32 i = 0; i < kCases; ++i) {
        if (!buildCase(i, boneCounts[i], cases_[i])) { shutdown(); return false; }
    }
    return true;
}

void SkinSelfTest::shutdown() {
    for (Case& c : cases_) {
        if (res_ && c.readback) res_->destroyBuffer(c.readback);
        pass_.destroyMesh(c.gpu);
        c = {};
    }
    pass_.shutdown();
    stage_ = 0;
    dev_ = nullptr;
    res_ = nullptr;
}

void SkinSelfTest::prePass(rhi::IRenderContext& ctx) {
    if (stage_ == 0) {
        // ALL THREE IN ONE FRAME, interleaved as a real scene would submit them. Dispatching them
        // across separate frames would restore exactly the blind spot this test exists to remove.
        for (Case& c : cases_) {
            if (!c.gpu.valid() || !c.readback) { stage_ = 2; return; }
            pass_.dispatch(ctx, c.gpu, c.skin.data(), static_cast<u32>(c.skin.size()));

            // dispatch left it in VertexBuffer, which is where a raster consumer wants it. The copy
            // needs CopySource, and the frame must END in Common because a buffer's state does not
            // survive the command list.
            skinTransition(ctx, c.gpu, rhi::ResourceState::CopySource);
            ctx.copyBuffer(c.readback, c.gpu.out,
                           static_cast<u64>(c.gpu.vertexCount) * kSkinVertexStride);
            skinTransition(ctx, c.gpu, rhi::ResourceState::Common);
        }
        stage_ = 1;
        return;
    }
    if (stage_ == 1) {
        // The copies were recorded LAST frame, and that frame has been submitted. waitIdle is what
        // makes the readback mean anything: readBuffer synchronises nothing by contract.
        res_->waitIdle();
        compare();
        stage_ = 2;
    }
}

void SkinSelfTest::compare() {
    const u32 stride = kSkinVertexStride / sizeof(f32);   // 8 floats: pos3, nrm3, uv2
    u32 worstVert = 0, worstCase = 0;
    bool readOk = true;

    for (u32 ci = 0; ci < kCases; ++ci) {
        Case& c = cases_[ci];
        std::vector<f32> gpu(static_cast<usize>(c.vertexCount) * stride);
        if (!res_->readBuffer(c.readback, gpu.data(), gpu.size() * sizeof(f32))) {
            AVER_ERROR("[Skin] self-test: readback failed for instance {}", ci);
            readOk = false;
            continue;
        }

        for (u32 v = 0; v < c.vertexCount; ++v) {
            const f32* g = &gpu[static_cast<usize>(v) * stride];
            for (u32 k = 0; k < 3; ++k) {
                const f32 dp = std::fabs(g[k]     - c.cpuPositions[v * 3 + k]);
                const f32 dn = std::fabs(g[3 + k] - c.cpuNormals[v * 3 + k]);
                if (dp > worstPos_) { worstPos_ = dp; worstVert = v; worstCase = ci; }
                if (dn > worstNrm_) worstNrm_ = dn;
            }
            // BIT-EXACT, not tolerant. The shader copies this field and never computes with it, so
            // any difference at all means the two sides disagree about where the field IS.
            if (g[6] != c.uvs[v * 2 + 0] || g[7] != c.uvs[v * 2 + 1]) ++uvMismatches_;
        }
    }

    passed_ = readOk && uvMismatches_ == 0 && worstPos_ <= kPosTolerance && worstNrm_ <= kNrmTolerance;
    if (passed_) {
        AVER_INFO("[Skin] self-test PASS: {} instances x {} vertices, worst position {:.6f} cm, "
                  "worst normal {:.6f}, uv carried through bit-exact",
                  kCases, kTestVerts, worstPos_, worstNrm_);
        return;
    }

    if (uvMismatches_)
        // Two causes reach here and the count tells them apart: a stride disagreement between
        // C++ and DXC corrupts uvs SPREAD ACROSS every instance, while a whole instance's worth
        // means that instance's output buffer was never written -- two dispatches crossed.
        AVER_ERROR("[Skin] self-test FAIL: {} of {} uvs came back changed. The shader only copies "
                   "this field, so either C++ and DXC disagree about the {}-byte element stride, or "
                   "an instance's output was never written; a count that is a clean multiple of {} "
                   "means the latter",
                   uvMismatches_, kCases * kTestVerts, kSkinVertexStride, kTestVerts);
    if (worstPos_ > kPosTolerance || worstNrm_ > kNrmTolerance) {
        const Case& c = cases_[worstCase];
        AVER_ERROR("[Skin] self-test FAIL: worst position {:.6f} cm at instance {} vertex {} "
                   "(tolerance {}), worst normal {:.6f} (tolerance {})",
                   worstPos_, worstCase, worstVert, kPosTolerance, worstNrm_, kNrmTolerance);
        AVER_ERROR("[Skin]   cpu ({:.4f}, {:.4f}, {:.4f}) -- a WRONG INSTANCE's answer here means "
                   "two dispatches shared a bone buffer or a binding set",
                   c.cpuPositions[worstVert * 3 + 0], c.cpuPositions[worstVert * 3 + 1],
                   c.cpuPositions[worstVert * 3 + 2]);
    }
}

} // namespace aver::render
