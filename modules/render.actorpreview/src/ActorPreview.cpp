#include "aver/render/preview/ActorPreview.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <cstring>

namespace aver::render::preview {
namespace {

constexpr f32 kPi = 3.14159265358979f;
f32 rad(f32 deg) { return deg * kPi / 180.0f; }
f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Row-major, row-vector, translation in the LAST ROW -- the engine's convention, stated because a
// preview that transposed here would put every model in a plausible wrong place.
void multiply(const f32 a[16], const f32 b[16], f32 out[16]) {
    f32 t[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            t[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                           a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
    std::memcpy(out, t, sizeof t);
}

// LEFT-HANDED look-at, +Z up, matching the engine contract. A right-handed one compiles, runs, and
// mirrors the actor -- which reads as the artist having modelled it backwards.
void lookAt(const f32 eye[3], const f32 at[3], f32 out[16]) {
    f32 f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    const f32 fl = std::sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    for (int i = 0; i < 3; ++i) f[i] = fl > 1e-6f ? f[i] / fl : (i == 0 ? 1.0f : 0.0f);

    const f32 up[3] = {0.0f, 0.0f, 1.0f};
    // right = up x forward, for a left-handed basis.
    f32 r[3] = {up[1]*f[2] - up[2]*f[1], up[2]*f[0] - up[0]*f[2], up[0]*f[1] - up[1]*f[0]};
    const f32 rl = std::sqrt(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
    // Looking straight down the pole leaves the cross product degenerate; the orbit clamp keeps the
    // camera off it, and this is the belt to that pair of braces.
    if (rl < 1e-5f) { r[0] = 1.0f; r[1] = 0.0f; r[2] = 0.0f; }
    else for (int i = 0; i < 3; ++i) r[i] /= rl;

    const f32 u[3] = {f[1]*r[2] - f[2]*r[1], f[2]*r[0] - f[0]*r[2], f[0]*r[1] - f[1]*r[0]};
    const f32 m[16] = {
        r[0], u[0], f[0], 0.0f,
        r[1], u[1], f[1], 0.0f,
        r[2], u[2], f[2], 0.0f,
        -(r[0]*eye[0] + r[1]*eye[1] + r[2]*eye[2]),
        -(u[0]*eye[0] + u[1]*eye[1] + u[2]*eye[2]),
        -(f[0]*eye[0] + f[1]*eye[1] + f[2]*eye[2]), 1.0f,
    };
    std::memcpy(out, m, sizeof m);
}

// REVERSED-Z is not used here: this target has its own depth buffer cleared to 1 and a Less test,
// independent of whatever the scene does. Stated so nobody "fixes" it to match the scene later.
void perspective(f32 fovDeg, f32 aspect, f32 nearZ, f32 farZ, f32 out[16]) {
    const f32 h = 1.0f / std::tan(rad(fovDeg) * 0.5f);
    const f32 w = h / aspect;
    const f32 m[16] = {
        w, 0, 0, 0,
        0, h, 0, 0,
        0, 0, farZ / (farZ - nearZ), 1,
        0, 0, -nearZ * farZ / (farZ - nearZ), 0,
    };
    std::memcpy(out, m, sizeof m);
}

} // namespace

void PreviewCamera::addOrbit(f32 dYaw, f32 dPitch) {
    yawDeg += dYaw;
    // Short of the pole, where the up vector flips and the view rolls over with no hysteresis --
    // the same failure the level camera's basis has at |z| > 0.95.
    pitchDeg = clampf(pitchDeg + dPitch, -85.0f, 85.0f);
}

void PreviewCamera::addZoom(f32 factor) {
    // Multiplicative, so a wheel notch moves the same PROPORTION at every scale. An additive zoom is
    // unusable across the range an actor can span -- a step that frames a 5 cm bolt puts a 20 m
    // vehicle in the next county.
    distance = clampf(distance * factor, 5.0f, 500000.0f);
}

const char* actorPreviewShaderSource() {
    // Compiled as the TAIL of the shared prelude, so VSIn/VSOut and the b0/b1 layouts come from
    // their one owner. What is added is a camera at b4 and a pair of entry points that read it.
    return R"(
// The preview's own camera, at the register RHIResources.hpp reserves for a feature. NOT b0: the
// backend rebinds slot 0 to the engine's block on every setPipeline, so a camera published there
// would survive exactly until the next pipeline change.
cbuffer PreviewFrame : register(b4) {
    float4x4 gPreviewViewProj;
    float4   gPreviewEye;      // xyz = eye, w = unused
    float4   gPreviewKey;      // xyz = direction TO the key light, w = its intensity
    float4   gPreviewAmbient;  // rgb = sky fill, w = selection highlight strength
};

struct PreviewOut {
    float4 pos   : SV_POSITION;
    float3 nrmWS : NORMAL;
    float3 wpos  : TEXCOORD0;
};

PreviewOut PreviewVS(VSIn i) {
    PreviewOut o;
    float4 wp = mul(float4(i.pos, 1.0), gWorld);
    o.wpos  = wp.xyz;
    o.pos   = mul(wp, gPreviewViewProj);
    o.nrmWS = normalize(mul(float4(i.nrm, 0.0), gWorld).xyz);
    return o;
}

float4 PreviewPS(PreviewOut i) : SV_TARGET {
    float3 n = normalize(i.nrmWS);
    float3 l = normalize(gPreviewKey.xyz);

    // A key light plus a hemisphere fill, and nothing else. This is a PREVIEW: it exists so an
    // author can read the shape and the placement of a part, and every additional term is one more
    // way for it to disagree with the level viewport it is already not trying to match.
    float ndl = saturate(dot(n, l));
    float3 base = gBaseColor.rgb;

    // The fill leans on the world +Z, so an upward face reads as sky-lit and a downward face as
    // ground-lit. Without it a shape's unlit side is flat black and its silhouette is unreadable,
    // which defeats the point of the view.
    float up = n.z * 0.5 + 0.5;
    float3 fill = lerp(gPreviewAmbient.rgb * 0.35, gPreviewAmbient.rgb, up);

    float3 lit = base * (fill + ndl * gPreviewKey.w);

    // A rim on the selection, added rather than replacing the colour: tinting the whole object hides
    // the material the author is looking at, and a rim survives against any base colour.
    float rim = pow(1.0 - saturate(dot(n, normalize(gPreviewEye.xyz - i.wpos))), 3.0);
    lit += gPreviewAmbient.w * rim * float3(1.0, 0.62, 0.2);

    return float4(toGamma(acesTonemap(lit)), 1.0);
}
)";
}

ActorPreview* ActorPreview::create(rhi::IDevice& device, u32 size) {
    auto* p = new ActorPreview();
    if (!p->init(device, size)) { delete p; return nullptr; }
    return p;
}

ActorPreview::~ActorPreview() {
    if (!res_) return;
    // Everything the UI could still be sampling. waitIdle first, because destroying a texture an
    // ImGui draw list still names is a use-after-free the validation layer does not see.
    res_->waitIdle();
    if (pipeline_) res_->destroyPipeline(pipeline_);
    if (vs_) res_->destroyShader(vs_);
    if (ps_) res_->destroyShader(ps_);
    if (color_) res_->destroyTexture(color_);
    if (depth_) res_->destroyTexture(depth_);
}

bool ActorPreview::init(rhi::IDevice& device, u32 size) {
    device_ = &device;
    res_ = device.resources();
    if (!res_) return false;   // no GPU backend; the editor shows the panel without a 3D view
    size_ = size ? size : 1024;

    rhi::TextureDesc cd;
    cd.width = cd.height = size_;
    cd.format = rhi::Format::RGBA8Unorm;   // NOT sRGB: the pixel shader gamma-encodes itself
    cd.bind = rhi::ResourceBind::RenderTarget | rhi::ResourceBind::ShaderResource;
    cd.initialState = rhi::ResourceState::ShaderResource;
    cd.hasClearValue = true;
    cd.debugName = "ActorPreview.Color";
    color_ = res_->createTexture(cd);

    rhi::TextureDesc dd;
    dd.width = dd.height = size_;
    dd.format = rhi::Format::D32Float;
    dd.bind = rhi::ResourceBind::DepthStencil;
    dd.initialState = rhi::ResourceState::DepthWrite;
    dd.hasClearValue = true;
    dd.clearDepth = 1.0f;
    dd.debugName = "ActorPreview.Depth";
    depth_ = res_->createTexture(dd);
    if (!color_ || !depth_) { AVER_ERROR("[Preview] could not create the preview targets"); return false; }

    rhi::ShaderDesc vd;
    vd.source = actorPreviewShaderSource();
    vd.prelude = rhi::sharedShaderPrelude();
    vd.entry = "PreviewVS";
    vd.stage = rhi::ShaderStage::Vertex;
    vs_ = res_->createShader(vd);

    rhi::ShaderDesc pd = vd;
    pd.entry = "PreviewPS";
    pd.stage = rhi::ShaderStage::Pixel;
    ps_ = res_->createShader(pd);
    if (!vs_ || !ps_) { AVER_ERROR("[Preview] the preview shaders would not compile"); return false; }

    rhi::GraphicsPipelineDesc gp;
    gp.vs = vs_;
    gp.ps = ps_;
    gp.cull = rhi::CullMode::Back;
    gp.depth = {true, true, rhi::CompareOp::Less};
    gp.renderTargetCount = 1;
    gp.renderTargets[0] = rhi::Format::RGBA8Unorm;
    gp.depthFormat = rhi::Format::D32Float;
    gp.sampleCount = 1;   // never multisampled: this target is sampled by the UI, not resolved
    // b1 is the per-draw block the shared prelude declares and drawMesh consumes; b4 is this
    // feature's camera. Slot 0 is left at zero dwords so the BACKEND binds the engine block there,
    // which is the contract -- a feature must never be able to observe b0 unbound.
    gp.layout.constantDwords[rhi::kObjectConstantRegister] = rhi::kObjectConstantDwords;
    gp.layout.constantDwords[rhi::kFeatureFrameConstantRegister] = 0;   // a root CBV
    pipeline_ = res_->createGraphicsPipeline(gp);
    if (!pipeline_) { AVER_ERROR("[Preview] the preview pipeline would not build"); return false; }

    uiTextureId_ = device.uiTextureId(color_);
    AVER_INFO("[Preview] ready: {}x{} target, pipeline {}", size_, size_, pipeline_);
    return true;
}

void ActorPreview::frameAll() {
    if (draws_.empty()) { camera_.distance = 400.0f; camera_.pivot[0] = camera_.pivot[1] = camera_.pivot[2] = 0.0f; return; }
    // Bounds of the placement ORIGINS, not of the geometry: the mesh extents are not known here (a
    // MeshHandle is opaque), and an actor's parts are placed at the points that matter. A margin
    // covers the geometry hanging off them.
    f32 lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const PreviewDraw& d : draws_) {
        // The mesh's own reach, scaled by the row lengths of its world matrix. Framing from the
        // translation alone put every class-level actor -- which has no placement, so its
        // translation is the origin -- at one identical distance, and a unit sphere came out as a
        // single white pixel. That is the bug this loop exists to not have.
        f32 sx = 0.0f, sy = 0.0f, sz = 0.0f;
        for (int k = 0; k < 3; ++k) {
            sx += d.world[0 + k] * d.world[0 + k];
            sy += d.world[4 + k] * d.world[4 + k];
            sz += d.world[8 + k] * d.world[8 + k];
        }
        const f32 scale = std::sqrt(std::fmax(sx, std::fmax(sy, sz)));
        const f32 r = d.boundsRadius * (scale > 0.0f ? scale : 1.0f);
        for (int i = 0; i < 3; ++i) {
            const f32 v = d.world[12 + i];   // translation is the LAST ROW
            if (v - r < lo[i]) lo[i] = v - r;
            if (v + r > hi[i]) hi[i] = v + r;
        }
    }
    f32 span = 0.0f;
    for (int i = 0; i < 3; ++i) {
        camera_.pivot[i] = (lo[i] + hi[i]) * 0.5f;
        span = std::fmax(span, hi[i] - lo[i]);
    }
    // Proportional to what is actually there, with a floor only for the degenerate case of a draw
    // list whose every mesh failed to resolve and therefore has no extent at all.
    camera_.distance = clampf(std::fmax(span, 1.0f) * 1.8f, 2.0f, 500000.0f);
}

void ActorPreview::buildViewProj(f32 out[16]) const {
    const f32 cy = std::cos(rad(camera_.yawDeg)), sy = std::sin(rad(camera_.yawDeg));
    const f32 cp = std::cos(rad(camera_.pitchDeg)), sp = std::sin(rad(camera_.pitchDeg));
    const f32 eye[3] = {
        camera_.pivot[0] - camera_.distance * cp * cy,
        camera_.pivot[1] - camera_.distance * cp * sy,
        camera_.pivot[2] + camera_.distance * sp,
    };
    f32 view[16], proj[16];
    lookAt(eye, camera_.pivot, view);
    // Near and far derived from the orbit distance rather than fixed: an actor framed at 20 cm and
    // one framed at 200 m cannot share a depth range without one of them z-fighting.
    perspective(camera_.fovDeg, 1.0f, std::fmax(camera_.distance * 0.01f, 0.5f),
                camera_.distance * 10.0f + 1000.0f, proj);
    multiply(view, proj, out);
}

void ActorPreview::prePass(rhi::IRenderContext& ctx) {
    if (!ready()) return;

    ctx.pushMarker("ActorPreview");

    // Into RenderTarget from wherever the last frame left it. The FIRST frame comes from the
    // creation state; every one after that from ShaderResource, because the UI sampled it.
    ctx.textureBarrier(color_, everRendered_ ? rhi::ResourceState::ShaderResource
                                             : rhi::ResourceState::ShaderResource,
                       rhi::ResourceState::RenderTarget);

    const rhi::TextureHandle targets[1] = {color_};
    ctx.setRenderTargets(targets, 1, depth_);
    ctx.setViewport(0, 0, size_, size_);
    // Set explicitly and not inherited: the editor leaves the scissor on its dock rect, which would
    // silently clip this pass to wherever the 3D view happens to be.
    ctx.setScissor(0, 0, size_, size_);
    ctx.clearDepth(depth_, 1.0f);

    ctx.setPipeline(pipeline_);

    struct Frame {
        f32 viewProj[16];
        f32 eye[4];
        f32 key[4];
        f32 ambient[4];
    } frame{};
    buildViewProj(frame.viewProj);

    const f32 cy = std::cos(rad(camera_.yawDeg)), sy = std::sin(rad(camera_.yawDeg));
    const f32 cp = std::cos(rad(camera_.pitchDeg)), sp = std::sin(rad(camera_.pitchDeg));
    frame.eye[0] = camera_.pivot[0] - camera_.distance * cp * cy;
    frame.eye[1] = camera_.pivot[1] - camera_.distance * cp * sy;
    frame.eye[2] = camera_.pivot[2] + camera_.distance * sp;

    // A FIXED three-quarter key, off the view axis. Fixed on purpose: a preview light that followed
    // the camera would light every face equally from every angle, which is the flat-lighting failure
    // the level's own sun default had -- an object needs a light it can turn relative to before its
    // form reads at all.
    frame.key[0] = -0.5481f; frame.key[1] = 0.3838f; frame.key[2] = 0.7431f; frame.key[3] = 1.6f;
    frame.ambient[0] = 0.26f; frame.ambient[1] = 0.30f; frame.ambient[2] = 0.36f;
    frame.ambient[3] = 0.0f;

    for (const PreviewDraw& d : draws_) {
        if (!d.mesh) continue;   // an unresolved mesh draws nothing rather than a fallback shape

        frame.ambient[3] = d.selected ? 0.9f : 0.0f;
        // Republished per draw, which setConstantBuffer makes free: it suballocates from the frame
        // ring, so this is a pointer bump rather than an upload.
        ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &frame, sizeof(frame));

        // The b1 block the shared prelude declares. Written whole, because setConstants always
        // overwrites the declared block and a partial write would inherit the previous draw's tail.
        f32 obj[rhi::kObjectConstantDwords] = {};
        std::memcpy(obj, d.world, sizeof(d.world));
        obj[16] = d.baseColor[0]; obj[17] = d.baseColor[1];
        obj[18] = d.baseColor[2]; obj[19] = d.baseColor[3];
        obj[20] = d.metallic; obj[21] = d.roughness; obj[22] = 0.0f; obj[23] = 0.0f;
        ctx.setConstants(rhi::kObjectConstantRegister, obj, rhi::kObjectConstantDwords);
        ctx.drawMesh(d.mesh);
    }

    // Back to ShaderResource so the UI can sample it, and so the next frame's barrier is true.
    ctx.textureBarrier(color_, rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderResource);
    everRendered_ = true;
    ctx.popMarker();
}

} // namespace aver::render::preview
