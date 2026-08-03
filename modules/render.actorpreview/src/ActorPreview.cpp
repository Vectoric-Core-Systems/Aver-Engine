// The actor preview feature: orbit camera, its own targets, and the HLSL it draws with.
#include "aver/render/preview/ActorPreview.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <cstring>

namespace aver::render::preview {
namespace {

constexpr f32 kPi = 3.14159265358979f;
f32 rad(f32 deg) { return deg * kPi / 180.0f; }
f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Multiplies two 4x4 matrices. Row-major, row-vector, translation in the last row.
void multiply(const f32 a[16], const f32 b[16], f32 out[16]) {
    f32 t[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            t[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                           a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
    std::memcpy(out, t, sizeof t);
}

// Builds a left-handed look-at view matrix, +Z up.
void lookAt(const f32 eye[3], const f32 at[3], f32 out[16]) {
    f32 f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    const f32 fl = std::sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    for (int i = 0; i < 3; ++i) f[i] = fl > 1e-6f ? f[i] / fl : (i == 0 ? 1.0f : 0.0f);

    const f32 up[3] = {0.0f, 0.0f, 1.0f};
    f32 r[3] = {up[1]*f[2] - up[2]*f[1], up[2]*f[0] - up[0]*f[2], up[0]*f[1] - up[1]*f[0]};
    const f32 rl = std::sqrt(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
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

// Builds a left-handed perspective projection. Not reversed-Z: this pass clears depth to 1 and tests Less.
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

// Turns the orbit, clamping pitch short of the pole.
void PreviewCamera::addOrbit(f32 dYaw, f32 dPitch) {
    yawDeg += dYaw;
    pitchDeg = clampf(pitchDeg + dPitch, -85.0f, 85.0f);
}

// Slides the pivot across the view plane by a mouse delta in screen pixels.
void PreviewCamera::panPixels(f32 dxPx, f32 dyPx, f32 viewportHeightPx) {
    if (viewportHeightPx < 1.0f) return;
    const f32 cy = std::cos(rad(yawDeg)), sy = std::sin(rad(yawDeg));
    const f32 cp = std::cos(rad(pitchDeg)), sp = std::sin(rad(pitchDeg));

    const f32 right[3] = {-sy, cy, 0.0f};
    const f32 up[3]    = {cy * sp, sy * sp, cp};

    // World centimetres per pixel at the pivot's depth.
    const f32 perPx = 2.0f * distance * std::tan(rad(fovDeg) * 0.5f) / viewportHeightPx;

    const f32 dx = -dxPx * perPx;
    const f32 dy =  dyPx * perPx;
    for (int i = 0; i < 3; ++i) pivot[i] += right[i] * dx + up[i] * dy;
}

// Scales the orbit distance by a factor, clamped.
void PreviewCamera::addZoom(f32 factor) {
    distance = clampf(distance * factor, 5.0f, 500000.0f);
}

// The preview's HLSL, compiled as the tail of the shared prelude.
const char* actorPreviewShaderSource() {
    return R"(
// The preview's own camera. Must be b4, the feature register: the backend rebinds b0 per setPipeline.
cbuffer PreviewFrame : register(b4) {
    float4x4 gPreviewViewProj;
    float4   gPreviewEye;      // xyz = eye, w = unused
    float4   gPreviewKey;      // xyz = direction TO the key light, w = its intensity
    float4   gPreviewAmbient;  // rgb = sky fill, w = selection highlight strength
};

// What the preview vertex shader hands the pixel shader.
struct PreviewOut {
    float4 pos   : SV_POSITION;
    float3 nrmWS : NORMAL;
    float3 wpos  : TEXCOORD0;
};

// Transforms a vertex to clip space and its normal to world space.
PreviewOut PreviewVS(VSIn i) {
    PreviewOut o;
    float4 wp = mul(float4(i.pos, 1.0), gWorld);
    o.wpos  = wp.xyz;
    o.pos   = mul(wp, gPreviewViewProj);
    o.nrmWS = normalize(averTransformNormal(i.nrm, gWorld));
    return o;
}

// Shades a pixel with one key light, a hemisphere fill and a selection rim.
float4 PreviewPS(PreviewOut i) : SV_TARGET {
    float3 n = normalize(i.nrmWS);
    float3 l = normalize(gPreviewKey.xyz);

    float ndl = saturate(dot(n, l));
    float3 base = gBaseColor.rgb;

    float up = n.z * 0.5 + 0.5;
    float3 fill = lerp(gPreviewAmbient.rgb * 0.35, gPreviewAmbient.rgb, up);

    float3 lit = base * (fill + ndl * gPreviewKey.w);

    float rim = pow(1.0 - saturate(dot(n, normalize(gPreviewEye.xyz - i.wpos))), 3.0);
    lit += gPreviewAmbient.w * rim * float3(1.0, 0.62, 0.2);

    return float4(toGamma(acesTonemap(lit)), 1.0);
}
)";
}

// Creates the feature. Returns null when the backend has no GPU support.
ActorPreview* ActorPreview::create(rhi::IDevice& device, u32 width, u32 height) {
    auto* p = new ActorPreview();
    if (!p->init(device, width, height)) { delete p; return nullptr; }
    return p;
}

// Waits for the GPU, then destroys the pipeline, shaders and targets.
ActorPreview::~ActorPreview() {
    if (!res_) return;
    res_->waitIdle();
    if (pipeline_) res_->destroyPipeline(pipeline_);
    if (vs_) res_->destroyShader(vs_);
    if (ps_) res_->destroyShader(ps_);
    if (color_) res_->destroyTexture(color_);
    if (depth_) res_->destroyTexture(depth_);
}

// Creates a colour and depth target pair at a size. Returns false if either fails.
bool ActorPreview::createTargets(u32 width, u32 height) {
    rhi::TextureDesc cd;
    cd.width = width;
    cd.height = height;
    cd.format = rhi::Format::RGBA8Unorm;   // NOT sRGB: the pixel shader gamma-encodes itself
    cd.bind = rhi::ResourceBind::RenderTarget | rhi::ResourceBind::ShaderResource;
    cd.initialState = rhi::ResourceState::ShaderResource;
    cd.hasClearValue = true;
    cd.debugName = "ActorPreview.Color";
    color_ = res_->createTexture(cd);

    rhi::TextureDesc dd;
    dd.width = width;
    dd.height = height;
    dd.format = rhi::Format::D32Float;
    dd.bind = rhi::ResourceBind::DepthStencil;
    dd.initialState = rhi::ResourceState::DepthWrite;
    dd.hasClearValue = true;
    dd.clearDepth = 1.0f;
    dd.debugName = "ActorPreview.Depth";
    depth_ = res_->createTexture(dd);
    if (!color_ || !depth_) { AVER_ERROR("[Preview] could not create the preview targets"); return false; }
    width_ = width;
    height_ = height;
    return true;
}

// Builds the targets, the shaders and the pipeline.
bool ActorPreview::init(rhi::IDevice& device, u32 width, u32 height) {
    device_ = &device;
    res_ = device.resources();
    if (!res_) return false;
    if (width == 0) width = 1024;
    if (height == 0) height = width;
    if (!createTargets(width, height)) return false;

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
    gp.sampleCount = 1;
    // Slot 0 stays at zero dwords so the backend binds the engine's per-frame block there.
    gp.layout.constantDwords[rhi::kObjectConstantRegister] = rhi::kObjectConstantDwords;
    gp.layout.constantDwords[rhi::kFeatureFrameConstantRegister] = 0;   // a root CBV
    pipeline_ = res_->createGraphicsPipeline(gp);
    if (!pipeline_) { AVER_ERROR("[Preview] the preview pipeline would not build"); return false; }

    uiTextureId_ = device.uiTextureId(color_);
    AVER_INFO("[Preview] ready: {}x{} target, pipeline {}", width_, height_, pipeline_);
    return true;
}

// Rebuilds the targets at a new size, keeping the old pair and returning false on failure.
bool ActorPreview::resize(u32 width, u32 height) {
    if (!res_ || width == 0 || height == 0) return false;
    if (width == width_ && height == height_) return true;

    res_->waitIdle();

    const rhi::TextureHandle oldColor = color_;
    const rhi::TextureHandle oldDepth = depth_;
    color_ = 0;
    depth_ = 0;
    if (!createTargets(width, height)) {
        color_ = oldColor;
        depth_ = oldDepth;
        AVER_WARN("[Preview] could not resize to {}x{}; keeping {}x{}", width, height, width_, height_);
        return false;
    }
    if (oldColor) res_->destroyTexture(oldColor);
    if (oldDepth) res_->destroyTexture(oldDepth);

    uiTextureId_ = device_ ? device_->uiTextureId(color_) : 0;
    everRendered_ = false;
    AVER_INFO("[Preview] target resized to {}x{}", width_, height_);
    return true;
}

// Points the camera at the whole draw list.
void ActorPreview::frameAll() {
    if (draws_.empty()) { camera_.distance = 400.0f; camera_.pivot[0] = camera_.pivot[1] = camera_.pivot[2] = 0.0f; return; }
    f32 lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const PreviewDraw& d : draws_) {
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
    camera_.distance = clampf(std::fmax(span, 1.0f) * 1.8f, 2.0f, 500000.0f);
}

// Composes the orbit camera's view and projection into one matrix.
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
    const f32 aspect = (width_ > 0 && height_ > 0)
                     ? static_cast<f32>(width_) / static_cast<f32>(height_) : 1.0f;
    perspective(camera_.fovDeg, aspect, std::fmax(camera_.distance * 0.01f, 0.5f),
                camera_.distance * 10.0f + 1000.0f, proj);
    multiply(view, proj, out);
}

// Draws the preview's targets: one barrier in, the draw list, one barrier back out for the UI.
void ActorPreview::prePass(rhi::IRenderContext& ctx) {
    if (!ready()) return;

    ctx.pushMarker("ActorPreview");

    ctx.textureBarrier(color_, everRendered_ ? rhi::ResourceState::ShaderResource
                                             : rhi::ResourceState::ShaderResource,
                       rhi::ResourceState::RenderTarget);

    const rhi::TextureHandle targets[1] = {color_};
    ctx.setRenderTargets(targets, 1, depth_);
    ctx.setViewport(0, 0, width_, height_);
    ctx.setScissor(0, 0, width_, height_);
    ctx.clearDepth(depth_, 1.0f);

    ctx.setPipeline(pipeline_);

    // Mirrors the PreviewFrame cbuffer field for field.
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

    // A fixed three-quarter key, off the view axis.
    frame.key[0] = -0.5481f; frame.key[1] = 0.3838f; frame.key[2] = 0.7431f; frame.key[3] = 1.6f;
    frame.ambient[0] = 0.26f; frame.ambient[1] = 0.30f; frame.ambient[2] = 0.36f;
    frame.ambient[3] = 0.0f;

    for (const PreviewDraw& d : draws_) {
        if (!d.mesh) continue;

        frame.ambient[3] = d.selected ? 0.9f : 0.0f;
        ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &frame, sizeof(frame));

        // The b1 block the shared prelude declares, written whole.
        f32 obj[rhi::kObjectConstantDwords] = {};
        std::memcpy(obj, d.world, sizeof(d.world));
        obj[16] = d.baseColor[0]; obj[17] = d.baseColor[1];
        obj[18] = d.baseColor[2]; obj[19] = d.baseColor[3];
        obj[20] = d.metallic; obj[21] = d.roughness; obj[22] = 0.0f; obj[23] = 0.0f;
        ctx.setConstants(rhi::kObjectConstantRegister, obj, rhi::kObjectConstantDwords);
        ctx.drawMesh(d.mesh);
    }

    ctx.textureBarrier(color_, rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderResource);
    everRendered_ = true;
    ctx.popMarker();
}

} // namespace aver::render::preview
