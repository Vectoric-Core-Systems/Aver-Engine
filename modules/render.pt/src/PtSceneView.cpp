// PtSceneView -- feeding the path tracer a real scene and getting its accumulator on screen. See
// PtSceneView.hpp for what this is, what it deliberately does not do, and why.
#include "aver/pt/PtSceneView.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"

#include <cmath>
#include <cstring>

namespace aver::pt {

namespace {

// Row-vector transform of a point (w = 1) through a projective matrix, so the perspective divide
// happens. IDENTICAL to the private helper VoxiRenderer.cpp already uses for the same job (fitting
// the shadow cascades to the camera frustum) -- not shared, because it is four lines and neither file
// wants a dependency on the other for it.
Vec3 xformProjected(const Vec3& p, const Mat4& m) {
    const f32 x = p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0];
    const f32 y = p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1];
    const f32 z = p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2];
    const f32 w = p.x * m.m[0][3] + p.y * m.m[1][3] + p.z * m.m[2][3] + m.m[3][3];
    const f32 inv = std::fabs(w) > 1e-9f ? 1.0f / w : 0.0f;
    return Vec3{x * inv, y * inv, z * inv};
}

// Splits a unit ray direction into (tan of its angle from `forward`, the unit axis it leans toward)
// -- the inverse of the PT shader's own ray-gen formula (PtShaders.hpp's CSPathTrace): dir =
// normalize(forward + axis*tan). False on a degenerate/behind-camera direction.
bool tanAndAxis(const Vec3& dir, const Vec3& forward, f32& tanOut, Vec3& axisOut) {
    const f32 cf = dot(dir, forward);
    if (!(cf > 1e-4f)) return false;
    const Vec3 perp = dir - forward * cf;
    axisOut = perp.getSafeNormal();
    if (axisOut.sizeSquared() < 0.5f) return false;
    tanOut = perp.size() / cf;
    return tanOut > 1e-4f;
}

// The presentation pixel shader: reads gPtAccum as a plain StructuredBuffer<float4> SRV (never a
// UAV -- PathTracer's own compute binding writes it as one; this is a SECOND binding set over the
// SAME buffer handle) and divides summed radiance by the sample count, the identical arithmetic
// PtFurnaceTest.hpp's Result::mean() does on the CPU, done here per pixel on the GPU. Runs through
// whatever tonemap the raster path's own post chain already applies (this writes into the SCENE
// colour target, BEFORE that chain, exactly where Voxi's own debug view writes), so the reference
// view is visually comparable rather than differently graded.
//
// SELF-CONTAINED, same reasoning AverSrFxaa.cpp gives for its own presentation shader: it declares
// its own tiny constant buffer and does NOT include rhi::sharedShaderPrelude() -- the vertex side
// borrows the prelude's VSky entry point (a plain SV_VertexID fullscreen triangle, the same one
// VoxiRenderer's own debug view compiles against), compiled as a SEPARATE shader object, so nothing
// here needs the prelude's own declarations.
constexpr const char* kPtPresentHLSL = R"(
struct PtPresentIn { float4 pos : SV_POSITION; float2 ndc : TEXCOORD0; };

cbuffer PtPresentCB : register(b1) { float4 gPtPresentInfo; };   // x width, y height, z layout, w unused

StructuredBuffer<float4> gPtAccumRead : register(t0);

float4 PSPathTracePresent(PtPresentIn i) : SV_TARGET {
    uint W = (uint)gPtPresentInfo.x;
    uint H = (uint)gPtPresentInfo.y;
    // i.ndc is [-1,1] with +Y toward the top of the viewport (D3D clip-space convention); the
    // accumulator's row 0 is ALSO the top of the traced image (see PtShaders.hpp's CSPathTrace: pixel
    // row 0 maps to ndc.y=-1 in ITS OWN convention, whose -ndc.y term then leans the ray toward +up --
    // i.e. row 0 leans toward whatever "up" the camera was handed). Flipping v here is what keeps the
    // two agreeing without needing a matching flip on the C++ side.
    float2 uv = i.ndc * 0.5 + 0.5;

    // BILINEAR, NOT NEAREST, AND THIS IS THE CHEAPEST REAL IMPROVEMENT ON THIS PATH.
    //
    // The accumulator is 480x270 (kAccumWidth/kAccumHeight) and the viewport it is shown in is
    // whatever the window is -- 3532x1987 on the display this was measured on, a 7.4x blow-up. The
    // old lookup truncated uv to an integer texel, so every accumulator texel became a solid ~7x7
    // block of identical pixels. That does not add noise, but it MAGNIFIES it: per-texel variance
    // that would read as fine film grain reads instead as coarse blocky mottling, which is far more
    // objectionable at the same numerical error. It also stair-stepped every silhouette in the
    // image, visible on the horizon line of any capture taken before this.
    //
    // Manual rather than a SamplerState because the accumulator is a StructuredBuffer, not a
    // texture -- it has to be, since the compute pass writes it as a UAV of float4 PAIRS (radiance,
    // and a sample count in .z). Four fetches instead of one, on a fullscreen pass that was already
    // trivially cheap.
    //
    // THE SAMPLE COUNT IS INTERPOLATED TOO, not taken from one texel. Every texel of a still frame
    // holds the same count so it makes no difference then -- but the frame after a camera move has
    // texels mid-update, and dividing one texel's radiance by another's count is how a blend seam
    // becomes a bright or dark band.
    float fx = clamp(uv.x * float(W) - 0.5, 0.0, float(W) - 1.0);
    float fy = clamp((1.0 - uv.y) * float(H) - 0.5, 0.0, float(H) - 1.0);
    uint x0 = (uint)floor(fx), y0 = (uint)floor(fy);
    uint x1 = min(x0 + 1u, W - 1u), y1 = min(y0 + 1u, H - 1u);
    float tx = fx - float(x0), ty = fy - float(y0);

    // TWO SOURCE LAYOUTS, chosen by gPtPresentInfo.z. The denoiser writes a one-element buffer of
    // means, so there is nothing left to divide; without it this reads the raw two-element
    // accumulator and divides, exactly as it did before the filter existed. Keeping the fallback
    // is what lets a device that cannot compile the filter still show a path-traced image.
    const bool denoised = gPtPresentInfo.z > 0.5;

    float3 c00, c10, c01, c11;
    if (denoised) {
        c00 = gPtAccumRead[y0 * W + x0].rgb;
        c10 = gPtAccumRead[y0 * W + x1].rgb;
        c01 = gPtAccumRead[y1 * W + x0].rgb;
        c11 = gPtAccumRead[y1 * W + x1].rgb;
    } else {
        // The sample count is interpolated with the radiance rather than taken from one texel:
        // the frame after a camera move has texels mid-update, and dividing one texel's radiance
        // by another's count is how a blend seam becomes a band.
        c00 = gPtAccumRead[(y0 * W + x0) * 2 + 0].rgb / max(gPtAccumRead[(y0 * W + x0) * 2 + 1].z, 1.0);
        c10 = gPtAccumRead[(y0 * W + x1) * 2 + 0].rgb / max(gPtAccumRead[(y0 * W + x1) * 2 + 1].z, 1.0);
        c01 = gPtAccumRead[(y1 * W + x0) * 2 + 0].rgb / max(gPtAccumRead[(y1 * W + x0) * 2 + 1].z, 1.0);
        c11 = gPtAccumRead[(y1 * W + x1) * 2 + 0].rgb / max(gPtAccumRead[(y1 * W + x1) * 2 + 1].z, 1.0);
    }

    return float4(lerp(lerp(c00, c10, tx), lerp(c01, c11, tx), ty), 1.0);
}
)";

// The DENOISER: one a-trous (Dammertz et al.) wavelet iteration per dispatch, over the accumulator.
// See PtSceneView.hpp's denoise block for why this exists and why its strength is driven by the
// per-pixel sample count rather than a fixed constant.
//
// TWO MODES IN ONE SHADER, chosen by gDnInfo.z, because they differ only in how the CENTRE and the
// TAPS are read:
//   0 RESOLVE -- the source is the two-element accumulator (summed radiance, then stats whose .z is
//                the sample count). Divides to a mean and writes float4(mean.rgb, sampleCount).
//   1 FILTER  -- the source is a one-element mean buffer in that same layout, written by a previous
//                pass of this shader.
// Carrying the sample count in .w rather than re-reading the accumulator is what lets every pass
// after the first ignore the accumulator entirely, and what lets the LAST pass still know how much
// to trust the pixel it is filtering.
constexpr const char* kPtDenoiseHLSL = R"(
cbuffer PtDenoiseCB : register(b1) {
    uint4  gDnInfo;   // x width, y height, z mode (0 resolve / 1 filter), w step in source texels
    float4 gDnTune;   // x colour sigma, y max samples, zw unused
};

StructuredBuffer<float4>   gDnSrc : register(t0);
RWStructuredBuffer<float4> gDnDst : register(u0);

float ptLum(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// One source texel as (mean radiance, sample count), whichever layout the source is in.
float4 ptDnFetch(uint x, uint y, uint W, uint mode) {
    const uint p = y * W + x;
    if (mode == 0u) {
        const float4 rad = gDnSrc[p * 2 + 0];
        const float  n   = max(gDnSrc[p * 2 + 1].z, 1.0);
        return float4(rad.rgb / n, n);
    }
    return gDnSrc[p];
}

[numthreads(8, 8, 1)]
void CSPtDenoise(uint3 tid : SV_DispatchThreadID) {
    const uint W = gDnInfo.x, H = gDnInfo.y;
    if (tid.x >= W || tid.y >= H) return;
    const uint mode = gDnInfo.z;
    const uint step = max(gDnInfo.w, 1u);

    const float4 c = ptDnFetch(tid.x, tid.y, W, mode);

    // RESOLVE ONLY CONVERTS. Filtering on the same pass would make the first iteration a special
    // case with a different support from the other two, for no gain -- the chain is short enough
    // that one extra dispatch over 130k pixels costs nothing worth counting.
    if (mode == 0u) { gDnDst[tid.y * W + tid.x] = c; return; }

    // A CONVERGED PIXEL IS PASSED THROUGH UNTOUCHED. The sigma below already tends to zero as the
    // sample count rises, so this changes no pixel that the filter would have altered meaningfully
    // -- it is here because this view exists to be a REFERENCE, and "the reference is bit-exact once
    // converged" is a property worth being able to state without qualification rather than one that
    // merely holds to several decimal places.
    if (c.a >= gDnTune.y) { gDnDst[tid.y * W + tid.x] = c; return; }

    // THE EDGE STOP. Expected Monte Carlo error falls as 1/sqrt(n), so the luminance difference that
    // counts as "still the same surface" is scaled by exactly that: wide on the frame after a camera
    // move, vanishing once the image has settled.
    //
    // RELATIVE TO LOCAL BRIGHTNESS, which is the half that matters for the problem this was built
    // for. A shadow sits near 0.1 luminance and the sunlit floor near 0.8; an absolute threshold
    // tuned on the floor does nothing in the shadow, and one tuned in the shadow flattens the floor.
    const float lc    = ptLum(c.rgb);
    const float sigma = gDnTune.x * rsqrt(max(c.a, 1.0)) * max(lc, 0.02);

    // B3 spline, the standard a-trous kernel: [1 4 6 4 1]/16 as an outer product.
    const float k[5] = { 0.0625, 0.25, 0.375, 0.25, 0.0625 };

    float3 sum = float3(0, 0, 0);
    float  wsum = 0.0;
    [unroll] for (int dy = -2; dy <= 2; ++dy) {
        [unroll] for (int dx = -2; dx <= 2; ++dx) {
            const int sx = int(tid.x) + dx * int(step);
            const int sy = int(tid.y) + dy * int(step);
            // CLAMPED, not skipped. Skipping would renormalise the kernel differently at the border
            // and leave a one-texel frame of differently-filtered pixels around the image.
            const uint qx = (uint)clamp(sx, 0, int(W) - 1);
            const uint qy = (uint)clamp(sy, 0, int(H) - 1);
            const float4 q = ptDnFetch(qx, qy, W, mode);
            const float wc = exp(-abs(ptLum(q.rgb) - lc) / (sigma + 1e-6));
            const float w  = k[dx + 2] * k[dy + 2] * wc;
            sum  += q.rgb * w;
            wsum += w;
        }
    }

    // wsum can never be zero -- the centre tap weighs k[2]*k[2] with wc == 1 -- but the guard costs
    // nothing and a NaN here would propagate through every later pass.
    gDnDst[tid.y * W + tid.x] = float4(wsum > 1e-6 ? sum / wsum : c.rgb, c.a);
}
)";

} // namespace

PtSceneView::~PtSceneView() { shutdown(); }

bool PtSceneView::init(rhi::IDevice& dev) {
    shutdown();
    dev_ = &dev;
    res_ = dev.resources();
    if (!res_) return false;
    if (!pt_.init(dev)) { shutdown(); return false; }
    AVER_INFO("[PT] scene view ready: {}x{} accumulator, {} bounce(s), {} spp/step, up to {} "
              "sample(s), up to {} static instance(s)",
              accumWidth_, accumHeight_, kMaxBounces, kSamplesPerStep, kMaxSamples, kMaxInstances);
    return true;
}

void PtSceneView::shutdown() {
    if (res_) {
        pt_.destroyTarget(target_);
        if (denoisePso_) res_->destroyPipeline(denoisePso_);
        for (rhi::BufferHandle& b : denoiseBuf_) { if (b) res_->destroyBuffer(b); b = 0; }
        if (denoiseSetResolve_) res_->destroyBindingSet(denoiseSetResolve_);
        if (denoiseSetAB_) res_->destroyBindingSet(denoiseSetAB_);
        if (denoiseSetBA_) res_->destroyBindingSet(denoiseSetBA_);
        if (presentSet_) res_->destroyBindingSet(presentSet_);
        if (presentPso_) res_->destroyPipeline(presentPso_);
    }
    presentSet_ = 0;
    denoisePso_ = 0;
    denoiseSetResolve_ = denoiseSetAB_ = denoiseSetBA_ = 0;
    denoiseReady_ = false;
    presentPso_ = 0;
    presentSampleCount_ = 0;
    presentColorFormat_ = rhi::Format::Unknown;
    pt_.shutdown();
    draws_.clear();
    drawsPrev_.clear();
    curCam_ = PtCamera{};
    haveCam_ = false;
    sceneKey_ = 0;
    sceneReady_ = false;
    sampleCursor_ = 0;
    loggedUnavailable_ = loggedFirstFrame_ = dropCapLogged_ = convergedLogged_ = false;
    dev_ = nullptr;
    res_ = nullptr;
}

void PtSceneView::beginScene() {
    drawsPrev_.swap(draws_);
    draws_.clear();
}

void PtSceneView::submitDraw(rhi::MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                             f32 metallic, f32 roughness, rhi::BindingSetHandle drawBinding,
                             const void* drawConstants, u32 drawConstantBytes) {
    (void)metallic; (void)roughness;
    if (mesh == 0) return;
    // STATIC ONLY -- see the class comment's GEOMETRY point. Same predicate
    // VoxiRenderer::buildAccelerationStructures already applies (gated on IDevice::meshVertexBuffer):
    // non-zero means this mesh's vertices are written by compute (skinned, or any future per-frame
    // vertex pass), which changes every frame -- including it would mean drawsKey() below never
    // settles and the accumulator would never progress past sample 0.
    if (dev_ && dev_->meshVertexBuffer(mesh) != 0) return;
    if (draws_.size() >= kMaxInstances) return;   // rebuildScene() reports the cap once; nothing to add here

    Draw d;
    d.mesh = mesh;
    std::memcpy(d.world, world, sizeof(d.world));

    // PREFER THE REAL MATERIAL'S BASE COLOUR OVER THE LEGACY PER-DRAW TINT -- BUT ONLY WHEN THE DRAW
    // IS ACTUALLY AUTHORED, AND ONLY THE HOST CAN SAY SO. Both halves of that were learned the hard
    // way and are worth keeping written down.
    //
    // "drawConstants is present and the right size" is NOT an authored signal. SandboxApp's scene-draw
    // loop calls setDrawBinding() UNCONDITIONALLY once `voxiRenderer_.materials().ready()`, with no
    // `if (authored)` guard -- so drawConstants is non-null and exactly sizeof(pbr::MaterialConstants)
    // on EVERY draw, authored or not, including every surface painted only through the legacy
    // surfaceLooks_ palette (which is what an ordinary hand-authored .ocmap with no .ocmat assets uses
    // for every surface). For those draws the block is MaterialSystem::fallbackConstants_, built from a
    // default-constructed MaterialDesc whose baseColorFactor is {1,1,1,1} -- so a "present => trust it"
    // rule reads WHITE for every non-authored surface regardless of its real colour. Caught by hand: a
    // red (0.86,0.20,0.16) box and a grey (0.48,0.50,0.55) wall, both painted only through
    // surfaceLooks_, traced IDENTICALLY pale blue-grey, differing only by shading.
    //
    // Nor is the block's SIZE an identity. sizeof(MaterialConstants) is 80 bytes and nothing in the RHI
    // stops another feature leaving an unrelated 80-byte block sticky in the same slot; it would be
    // read as a material, silently, and the only symptom would be a wrong colour. The size test held
    // only because every producer today happens to bind a real material -- an invariant nothing
    // enforces and no build would catch breaking.
    //
    // So the question goes to the HOST, which is the only layer that knows what it bound (see
    // setAlbedoResolver, and LandscapeRenderer::setSurfaceBinding for the same division of labour).
    // It answers from the binding's IDENTITY rather than from the bytes' shape. Absent a resolver --
    // PtFurnaceTest, or any host with no material system -- every draw keeps its base colour.
    f32 resolved[3];
    if (resolveAlbedo_ && resolveAlbedo_(drawBinding, drawConstants, drawConstantBytes, resolved)) {
        d.albedo[0] = resolved[0];
        d.albedo[1] = resolved[1];
        d.albedo[2] = resolved[2];
    } else {
        d.albedo[0] = baseColor[0];
        d.albedo[1] = baseColor[1];
        d.albedo[2] = baseColor[2];
    }
    draws_.push_back(d);
}

u64 PtSceneView::drawsKey() const {
    // FNV-1a, the identical constants and shape VoxiRenderer::giDrawsKey() uses for the same reason:
    // order-sensitive and bit-exact over the raw float bits, not a tolerant comparison. A cache whose
    // invalidation rule is "close enough" is a cache that goes stale silently.
    u64 key = 1469598103934665603ull;
    for (const Draw& d : drawsPrev_) {
        key ^= static_cast<u64>(d.mesh);
        key *= 1099511628211ull;
        for (u32 i = 0; i < 16; ++i) {
            u32 bits = 0;
            std::memcpy(&bits, &d.world[i], sizeof(bits));
            key ^= static_cast<u64>(bits);
            key *= 1099511628211ull;
        }
        for (u32 i = 0; i < 3; ++i) {
            u32 bits = 0;
            std::memcpy(&bits, &d.albedo[i], sizeof(bits));
            key ^= static_cast<u64>(bits);
            key *= 1099511628211ull;
        }
    }
    return key;
}

bool PtSceneView::deriveCamera(PtCamera& out) const {
    // viewProj itself is never read: the combined matrix cannot be decomposed back into view and
    // projection alone (see the class comment), so everything below works from invViewProj and the
    // eye position instead. IDevice::camera() allows a null output for exactly this reason.
    f32 invVp[16], eye[3];
    if (!dev_ || !dev_->camera(nullptr, invVp, eye)) return false;
    Mat4 invVP;
    std::memcpy(&invVP.m[0][0], invVp, sizeof(invVP.m));
    const Vec3 eyeV{eye[0], eye[1], eye[2]};

    // Three points on the far plane: NDC centre, top-centre and right-centre. Unprojecting THROUGH
    // the far plane and subtracting the known eye gives a world-space ray direction without needing
    // the view and projection matrices separately -- which IDevice::camera() does not hand back (see
    // the class comment: the combined viewProj cannot be decomposed into the two alone).
    const Vec3 farCenter = xformProjected(Vec3{0.0f, 0.0f, 1.0f}, invVP);
    const Vec3 farTop    = xformProjected(Vec3{0.0f, 1.0f, 1.0f}, invVP);
    const Vec3 farRight  = xformProjected(Vec3{1.0f, 0.0f, 1.0f}, invVP);

    const Vec3 forward = (farCenter - eyeV).getSafeNormal();
    if (forward.sizeSquared() < 0.5f) return false;   // no usable camera yet

    const Vec3 dirTop   = (farTop   - eyeV).getSafeNormal();
    const Vec3 dirRight = (farRight - eyeV).getSafeNormal();

    f32 tanV = 0.0f, tanH = 0.0f;
    Vec3 up, right;
    if (!tanAndAxis(dirTop, forward, tanV, up)) return false;
    if (!tanAndAxis(dirRight, forward, tanH, right)) return false;
    (void)tanH;   // the real camera's horizontal FOV; deliberately NOT used, see aspect below

    out = PtCamera{};
    out.origin[0] = eyeV.x;  out.origin[1] = eyeV.y;  out.origin[2] = eyeV.z;
    out.forward[0] = forward.x; out.forward[1] = forward.y; out.forward[2] = forward.z;
    out.right[0] = right.x;  out.right[1] = right.y;  out.right[2] = right.z;
    out.up[0] = up.x;        out.up[1] = up.y;        out.up[2] = up.z;
    out.tanHalfFov = tanV;
    // THE ACCUMULATOR'S OWN ASPECT, NEVER THE REAL VIEWPORT'S. A small fixed reference target is not
    // obliged to match whatever ratio the editor happens to be docked at; deriving it from the real
    // viewport would stretch this image the moment the two disagree, and the editor's dockspace rect
    // changes size far more often than the level does.
    out.aspect = static_cast<f32>(accumWidth_) / static_cast<f32>(accumHeight_);
    return true;
}

bool PtSceneView::rebuildScene(rhi::IRenderContext& ctx) {
    pt_.destroyTarget(target_);
    pt_.resetScene();
    sceneReady_ = false;

    std::vector<u32> ids;
    ids.reserve(drawsPrev_.size());
    u32 dropped = 0;
    for (const Draw& d : drawsPrev_) {
        if (ids.size() >= kMaxInstances) { ++dropped; continue; }
        PtSurface s;
        s.mesh = d.mesh;
        std::memcpy(s.world, d.world, sizeof(s.world));
        std::memcpy(s.albedo, d.albedo, sizeof(s.albedo));
        ids.push_back(pt_.addSurface(s));
    }
    if (dropped && !dropCapLogged_) {
        dropCapLogged_ = true;
        AVER_WARN("[PT] scene view: {} static instance(s) this frame, only the first {} are traced "
                  "-- raise PtSceneView::kMaxInstances", ids.size() + dropped, kMaxInstances);
    }
    if (ids.empty()) return false;   // an empty scene: everything was filtered, or there is nothing yet

    pt_.addScene(ids.data(), static_cast<u32>(ids.size()));
    if (!pt_.prepare()) {
        AVER_ERROR("[PT] scene view: the flat geometry table could not be built");
        return false;
    }
    if (!pt_.createTarget(0, accumWidth_, accumHeight_, target_)) {
        AVER_ERROR("[PT] scene view: could not allocate the accumulator");
        return false;
    }
    if (!pt_.buildScenes(ctx)) {
        AVER_ERROR("[PT] scene view: the acceleration structures would not build");
        pt_.destroyTarget(target_);
        return false;
    }
    if (!ensurePresentResources()) return false;

    sceneReady_ = true;
    convergedLogged_ = false;
    AVER_INFO("[PT] scene view: re-armed on {} static instance(s)", static_cast<u32>(ids.size()));
    return true;
}

bool PtSceneView::ensureDenoiseResources() {
    if (!res_ || !target_.valid()) return false;

    if (!denoisePso_) {
        rhi::ShaderDesc sd;
        sd.source = kPtDenoiseHLSL;
        sd.entry  = "CSPtDenoise";
        sd.stage  = rhi::ShaderStage::Compute;
        // NO PRELUDE, unlike the integrator: this shader touches no engine concept at all -- no sky,
        // no material, no PI. It is arithmetic over a buffer, and including the prelude would bind it
        // to declarations it never reads.
        const rhi::ShaderHandle cs = res_->createShader(sd);
        if (cs) {
            rhi::ComputePipelineDesc pd;
            pd.cs = cs;
            pd.layout.srvCount = 1;
            pd.layout.uavCount = 1;
            pd.layout.constantDwords[kPresentConstantRegister] = 0;
            denoisePso_ = res_->createComputePipeline(pd);
            res_->destroyShader(cs);
        }
        if (!denoisePso_) {
            // NOT FATAL. Presentation falls back to reading the accumulator directly, which is
            // exactly what it did before this existed -- a device that cannot compile the filter
            // still gets a path-traced image, just a noisier one.
            AVER_WARN("[PT] scene view: the denoiser would not compile; presenting unfiltered");
            return false;
        }
    }

    const u32 pixels = target_.pixels();
    if (!denoiseBuf_[0]) {
        for (u32 i = 0; i < 2; ++i) {
            rhi::BufferDesc bd;
            bd.bytes = static_cast<u64>(pixels) * kPtAccumStride;   // one float4 per pixel
            bd.kind  = rhi::BufferKind::Default;
            bd.allowUnorderedAccess = true;
            bd.debugName = i == 0 ? "pt denoise A" : "pt denoise B";
            denoiseBuf_[i] = res_->createBuffer(bd);
            if (!denoiseBuf_[i]) {
                AVER_ERROR("[PT] scene view: could not allocate the denoise buffers");
                return false;
            }
        }
    }

    const auto makeSet = [&](rhi::BindingSetHandle& set) {
        if (set) return true;
        rhi::BindingSetDesc bd;
        bd.srvCount = 1;
        bd.uavCount = 1;
        bd.srvKinds[0] = rhi::SlotKind::StructuredBuffer;
        bd.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
        set = res_->createBindingSet(bd);
        return set != 0;
    };
    if (!makeSet(denoiseSetResolve_) || !makeSet(denoiseSetAB_) || !makeSet(denoiseSetBA_)) {
        AVER_ERROR("[PT] scene view: denoise binding sets unavailable");
        return false;
    }

    // Rewritten every rebuild, because rebuildScene() always hands back a NEW accumulator handle --
    // the same reason ensurePresentResources() rewrites its SRV rather than creating a set per target.
    res_->setSrvBuffer(denoiseSetResolve_, 0, target_.accum, kPtAccumStride,
                       pixels * kPtAccumElementsPerPixel, 0);
    res_->setUavBuffer(denoiseSetResolve_, 0, denoiseBuf_[0], kPtAccumStride, pixels, 0);
    res_->setSrvBuffer(denoiseSetAB_, 0, denoiseBuf_[0], kPtAccumStride, pixels, 0);
    res_->setUavBuffer(denoiseSetAB_, 0, denoiseBuf_[1], kPtAccumStride, pixels, 0);
    res_->setSrvBuffer(denoiseSetBA_, 0, denoiseBuf_[1], kPtAccumStride, pixels, 0);
    res_->setUavBuffer(denoiseSetBA_, 0, denoiseBuf_[0], kPtAccumStride, pixels, 0);

    // WHICH BUFFER HOLDS THE ANSWER IS A PARITY, not something the dispatch loop discovers. The
    // chain starts at A and alternates once per pass, so after kDenoisePasses it is in A for an
    // even count and B for an odd one. Deciding it HERE, at descriptor-write time, is what lets
    // the present set be written once per rebuild rather than once per frame -- rewriting a
    // descriptor the GPU may still be reading from the previous frame is the hazard
    // PathTracer::createTarget's own comment warns about, and this sidesteps it entirely.
    denoiseResult_ = kDenoisePasses % 2u;

    denoiseReady_ = true;
    return true;
}

void PtSceneView::runDenoise(rhi::IRenderContext& ctx) {
    if (!denoiseReady_ || !denoisePso_ || !target_.valid()) return;

    struct DenoiseCB { u32 info[4]; f32 tune[4]; } cb{};
    cb.info[0] = target_.width;
    cb.info[1] = target_.height;
    cb.tune[0] = kDenoiseColorSigma;
    cb.tune[1] = static_cast<f32>(kMaxSamples);

    const u32 gx = (target_.width  + 7) / 8;
    const u32 gy = (target_.height + 7) / 8;

    ctx.pushMarker("Aver.PtDenoise");

    // RESOLVE: accumulator -> A. The accumulator is read as an SRV here and written as a UAV by
    // accumulate(); both bindings name the same buffer, which is why it has to be walked back to
    // Common by whoever touched it last (PathTracer::accumulate does).
    // THE ACCUMULATOR IS READ HERE AS AN SRV and was last written as a UAV, so it needs an explicit
    // transition -- the RHI tracks buffer state and does not model D3D12's implicit Common promotion,
    // the same reason scenePass() barriers what it samples. It happened to read correctly without
    // this on the device it was written on, which is exactly the kind of luck that stops being luck
    // on a different driver.
    ctx.bufferBarrier(target_.accum, rhi::ResourceState::Common, rhi::ResourceState::ShaderResource);
    ctx.bufferBarrier(denoiseBuf_[0], rhi::ResourceState::Common, rhi::ResourceState::UnorderedAccess);
    ctx.setPipeline(denoisePso_);
    ctx.setBindingSet(denoiseSetResolve_);
    cb.info[2] = 0;   // resolve
    cb.info[3] = 1;
    ctx.setConstantBuffer(kPresentConstantRegister, &cb, sizeof(cb));
    ctx.dispatch(gx, gy, 1);
    ctx.bufferBarrier(denoiseBuf_[0], rhi::ResourceState::UnorderedAccess, rhi::ResourceState::Common);
    ctx.bufferBarrier(target_.accum, rhi::ResourceState::ShaderResource, rhi::ResourceState::Common);

    // FILTER: A -> B -> A -> ..., step doubling each pass. Each dispatch must SEE the previous one's
    // writes, so the barriers are per pass and not hoisted out of the loop -- an a-trous chain whose
    // passes overlap reads and writes of the same texels is not a wavelet transform, it is a race.
    u32 src = 0;
    for (u32 i = 0; i < kDenoisePasses; ++i) {
        const u32 dst = 1u - src;
        ctx.bufferBarrier(denoiseBuf_[dst], rhi::ResourceState::Common, rhi::ResourceState::UnorderedAccess);
        ctx.setBindingSet(src == 0 ? denoiseSetAB_ : denoiseSetBA_);
        cb.info[2] = 1;          // filter
        cb.info[3] = 1u << i;    // 1, 2, 4 -- see kDenoisePasses
        ctx.setConstantBuffer(kPresentConstantRegister, &cb, sizeof(cb));
        ctx.dispatch(gx, gy, 1);
        ctx.bufferBarrier(denoiseBuf_[dst], rhi::ResourceState::UnorderedAccess, rhi::ResourceState::Common);
        src = dst;
    }
    // src is now kDenoisePasses % 2, which is what ensureDenoiseResources already bound. Checked at
    // COMPILE time rather than assigned, so changing kDenoisePasses can never silently present a
    // stale buffer -- there is nothing about this a run could discover that the constant does not
    // already determine.
    static_assert(kDenoisePasses % 2u == 1u,
                  "the present set is bound to buffer B; an even pass count would leave the result in A");
    (void)src;
    ctx.popMarker();
}

bool PtSceneView::ensurePresentResources() {
    if (!res_) return false;
    if (!presentSet_) {
        rhi::BindingSetDesc bd;
        bd.srvCount = 1;
        bd.srvKinds[0] = rhi::SlotKind::StructuredBuffer;
        presentSet_ = res_->createBindingSet(bd);
        if (!presentSet_) {
            AVER_ERROR("[PT] scene view: present binding set unavailable");
            return false;
        }
    }
    // ONE BINDING SET REWRITTEN IN PLACE, not one per target, unlike PathTracer::createTarget()'s own
    // reasoning for a fresh set per scene -- there, TWO recorded dispatches could read the same shared
    // set at different times within one frame; here scenePass() runs at most once per frame and always
    // AFTER this rewrite for the frame's current target_, so there is nothing for two reads to race.
    // AFTER the denoiser has been given its chance, so the SRV names the buffer that will actually
    // hold this frame's answer. ensureDenoiseResources() reports its own failure and leaves
    // denoiseReady_ false, which is the fallback path -- not an error for the view as a whole.
    ensureDenoiseResources();
    if (denoiseReady_) {
        res_->setSrvBuffer(presentSet_, 0, denoiseBuf_[denoiseResult_], kPtAccumStride,
                           target_.pixels(), 0);
    } else {
        res_->setSrvBuffer(presentSet_, 0, target_.accum, kPtAccumStride,
                           target_.pixels() * kPtAccumElementsPerPixel, 0);
    }
    return true;
}

void PtSceneView::setQuality(u32 rung) {
    const u32 clamped = rung < 4 ? rung : 3;
    if (clamped == quality_) return;   // idempotent: safe to call every frame
    quality_ = clamped;
    accumWidth_  = kAccumLadder[clamped].width;
    accumHeight_ = kAccumLadder[clamped].height;
    // Forces prePass's re-arm branch, which is the only place createTarget() is called and
    // therefore the only place a differently-sized accumulator can come into existence. Clearing
    // sceneReady_ rather than the key means this works even when the scene itself has not changed.
    sceneReady_ = false;
    sampleCursor_ = 0;
    loggedFirstFrame_ = convergedLogged_ = false;
    AVER_INFO("[PT] scene view: quality rung {} -- accumulator {}x{}", quality_, accumWidth_, accumHeight_);
}

void PtSceneView::prePass(rhi::IRenderContext& ctx) {
    if (!dev_ || !pt_.available()) return;
    // COLD START: prePass() runs at the TOP of beginFrame(), before this run's onRender() has made
    // any drawMesh() call at all, so drawsPrev_ (last frame's fully-known list) is genuinely empty on
    // frame 1 -- not because the scene is empty, but because no frame has finished submitting yet.
    // Waiting here for the first non-empty list is what keeps "nothing static to trace" below a real
    // finding (every draw filtered, or the device refused a structure) rather than a false alarm on
    // every single run's first frame.
    if (drawsPrev_.empty() && !sceneReady_) return;

    // ---- 1. does the STATIC scene need a full re-arm? ----
    const u64 key = drawsKey();
    const bool needRearm = !sceneReady_ || key != sceneKey_;
    if (needRearm) {
        sceneKey_ = key;
        if (!rebuildScene(ctx)) {
            if (!loggedUnavailable_) {
                loggedUnavailable_ = true;
                AVER_WARN("[PT] scene view: nothing static to trace this frame (every draw was "
                          "filtered, or the device refused an acceleration structure) -- staying "
                          "black until the scene changes");
            }
            return;
        }
        loggedUnavailable_ = false;
        haveCam_ = false;   // forces the camera branch below to also reset the accumulator
    }

    // ---- 2. has the camera moved? ----
    PtCamera cam;
    if (!deriveCamera(cam)) return;   // no camera to read yet this run
    const bool camChanged = !haveCam_ || std::memcmp(&cam, &curCam_, sizeof(PtCamera)) != 0;
    if (camChanged) {
        curCam_ = cam;
        haveCam_ = true;
        sampleCursor_ = 0;
        convergedLogged_ = false;
    }

    // ---- 3. accumulate one bounded step, unless this image has already converged ----
    if (sampleCursor_ >= kMaxSamples) {
        if (!convergedLogged_) {
            convergedLogged_ = true;
            AVER_INFO("[PT] scene view: converged at {} samples/pixel -- camera still, no further "
                      "work issued until it moves", sampleCursor_);
        }
        return;
    }

    // BOUNDED, PER THIS MODULE'S OWN GPU-HYGIENE RULE. One accumulate() call issues roughly
    // (bounces+1)*samples = 5*8 = 40 RayQuery traces per pixel, over 480x270 pixels: ~5.2M traces.
    // VoxiRenderer's own ray-traced sun shadow -- the only OTHER ray-traced pass in this engine, and
    // one already characterised against a recorded TDR history on this machine -- defaults to 4 rays
    // per pixel over the FULL scene resolution (VoxiRenderer.hpp: rtShadowRays_, capped at 32
    // "because there is a recorded TDR history on this machine"): at a modest 1280x720 scene that is
    // 4 * 921,600 = ~3.7M traces/frame. This dispatch is the same order of magnitude, not a multiple
    // of it, and unlike the shadow pass it is NOT issued every frame once the image has converged
    // (see the kMaxSamples check above) or while the camera is moving (see the reset above, which
    // always sets d.reset=true on the FIRST call after a move, discarding whatever partial sum a
    // half-issued dispatch would otherwise smear across a new view).
    PtDispatch d;
    d.maxBounces = kMaxBounces;
    d.samples = kSamplesPerStep;
    d.firstSample = sampleCursor_;
    d.reset = (sampleCursor_ == 0);
    pt_.accumulate(ctx, target_, curCam_, d);
    // Immediately after, in the same pass: the filter reads what accumulate() just wrote, and a
    // frame that accumulated without re-filtering would present the PREVIOUS sample count's
    // image. Skipped entirely on a converged frame, along with the accumulate() above it -- the
    // denoise buffers persist, so the last result is still the right one.
    runDenoise(ctx);
    sampleCursor_ += kSamplesPerStep;

    if (!loggedFirstFrame_) {
        loggedFirstFrame_ = true;
        AVER_INFO("[PT] scene view: tracing at {}x{}, {} bounce(s), {} spp/step, converges at {} "
                  "samples/pixel", target_.width, target_.height, kMaxBounces, kSamplesPerStep,
                  kMaxSamples);
    }
}

void PtSceneView::scenePass(rhi::IRenderContext& ctx) {
    if (!presentPso_ || !presentSet_ || !sceneReady_ || !target_.valid()) return;
    rhi::ScopedGpuStat gpuStat(ctx, "PT scene view present");
    // accumulate() leaves the buffer in Common (see PathTracer::accumulate's own comment: a buffer's
    // state does not survive the command list). Reading it here as a pixel-shader SRV needs an
    // EXPLICIT transition -- the RHI tracks buffer state and does not model D3D12's implicit Common
    // promotion, the same reasoning PathTracer::buildScenes gives for its own explicit vertex/index
    // barriers.
    // The buffer being READ is whichever one the present set was pointed at -- the denoiser's
    // output when it is running, the accumulator when it is not. Barriering the accumulator while
    // reading a different buffer would leave the one actually being sampled in the wrong state.
    const rhi::BufferHandle shown = denoiseReady_ ? denoiseBuf_[denoiseResult_] : target_.accum;
    ctx.bufferBarrier(shown, rhi::ResourceState::Common, rhi::ResourceState::ShaderResource);
    ctx.setPipeline(presentPso_);
    ctx.setBindingSet(presentSet_);
    const f32 info[4] = {static_cast<f32>(target_.width), static_cast<f32>(target_.height),
                          denoiseReady_ ? 1.0f : 0.0f, 0.0f};
    ctx.setConstantBuffer(kPresentConstantRegister, info, sizeof(info));
    ctx.drawFullscreen();
    // Back to Common, matching every other consumer of these buffers (accumulate(), runDenoise(),
    // copyForReadback()): the NEXT dispatch assumes it starts there.
    ctx.bufferBarrier(shown, rhi::ResourceState::ShaderResource, rhi::ResourceState::Common);
}

void PtSceneView::onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth,
                                         u32 width, u32 height) {
    (void)depth; (void)width; (void)height;
    if (!res_) return;
    if (presentPso_ && sampleCount == presentSampleCount_ && color == presentColorFormat_) return;
    if (presentPso_) { res_->destroyPipeline(presentPso_); presentPso_ = 0; }

    // VSky: the SAME fullscreen-triangle vertex shader VoxiRenderer's own debug view compiles against
    // (VoxiRenderer.cpp's createScenePipelines, "shares the prelude's fullscreen triangle"), read out
    // of the shared prelude rather than duplicated here.
    // SM 5.1, the SAME minimum VoxiRenderer's own VSky compile uses (VoxiRenderer.cpp's kBaseSm) and
    // AverSrFxaa's presentation shaders use (AverSrFxaa.cpp) -- a fullscreen triangle and a
    // StructuredBuffer read need nothing from SM6, and asking for less than the integrator itself
    // needs is what keeps this pass working on the same hardware floor as everything else it stands
    // beside, even though the integrator compute shader beside it is pinned to SM 6.5 for RayQuery.
    rhi::ShaderDesc vsd;
    vsd.source = rhi::sharedShaderPrelude();
    vsd.entry = "VSky";
    vsd.stage = rhi::ShaderStage::Vertex;
    vsd.minShaderModel = 51;
    rhi::ShaderDesc psd;
    psd.source = kPtPresentHLSL;
    psd.entry = "PSPathTracePresent";
    psd.stage = rhi::ShaderStage::Pixel;
    psd.minShaderModel = 51;
    const rhi::ShaderHandle vs = res_->createShader(vsd);
    const rhi::ShaderHandle ps = res_->createShader(psd);

    bool ok = false;
    if (vs && ps) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vs;
        p.ps = ps;
        p.layout.srvCount = 1;   // t0: gPtAccumRead
        p.cull = rhi::CullMode::None;
        p.renderTargetCount = 1;
        p.renderTargets[0] = color;
        p.sampleCount = sampleCount;
        presentPso_ = res_->createGraphicsPipeline(p);
        ok = presentPso_ != 0;
    }
    if (vs) res_->destroyShader(vs);
    if (ps) res_->destroyShader(ps);

    if (!ok) AVER_ERROR("[PT] scene view: present pipeline unavailable");
    presentSampleCount_ = sampleCount;
    presentColorFormat_ = color;
}

} // namespace aver::pt
