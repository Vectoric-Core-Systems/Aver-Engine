// The actor preview feature: orbit camera, its own targets, and the HLSL it draws with.
#include "aver/render/preview/ActorPreview.hpp"
#include "aver/core/Log.hpp"

#if AVER_MODULE_PBR
#include "aver/pbr/MaterialGpu.hpp"
#include "aver/pbr/MaterialGraphRegistry.hpp"
#include "aver/pbr/PbrShaders.hpp"
#endif

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

#if AVER_MODULE_PBR
// The prelude the material pipeline's shaders compile against: the shared RHI declarations, the
// material system's BRDF and Aver* contract, then whatever the process's graphs currently compile
// to. Mirrors VoxiRenderer::voxiShaderPrelude() in modules/render.voxi/src/VoxiRenderer.cpp --
// read that comment for why AVER_MATERIAL_GRAPH is text pasted between the two preludes rather
// than a -D (a -D would have to be repeated on every shader stage sharing this prelude, and the
// one stage that got missed would fail to link with a duplicate-function error nowhere near the
// cause) and for why this rebuilds on the registry's REVISION rather than once at construction or
// on every frame: graphs load long after this feature exists, and the common case -- no graphs at
// all, which is every project that exists today -- must rebuild nothing and cost nothing.
const char* actorPreviewMaterialPrelude() {
    static std::string s;
    static u64 built = ~0ull;
    const u64 rev = pbr::materialGraphs().revision();
    if (built != rev) {
        s = std::string(rhi::sharedShaderPrelude());
        s += "\n#define AVER_MATERIAL_GRAPH 1\n";
        s += pbr::materialShaderPrelude();
        s += pbr::materialGraphs().hlsl();
        built = rev;
    }
    return s.c_str();
}

// The material pipeline's OWN pixel entry point, built on top of actorPreviewShaderSource() rather
// than folded into it. THAT SEPARATION IS THE WHOLE POINT: PreviewMaterialPS references AverVertex,
// AverLight and averEvalMaterial, none of which exist unless a compile also carries
// actorPreviewMaterialPrelude() above -- so this text must never reach the SIMPLE pipeline's compile,
// which shares actorPreviewShaderSource() with every caller that predates this feature and must
// keep compiling against nothing but rhi::sharedShaderPrelude(). Appending here, in a string this
// simple pipeline's own ShaderDesc never references, is what keeps that true without a single
// #ifdef inside the shared source -- and it is also why ActorPreviewTest can assert "no shader
// source mentions AVER_MATERIAL_GRAPH" for the no-graph case and mean it literally: the macro name
// never appears in ANY .source string, only in this prelude's own text.
const char* actorPreviewMaterialShaderSource() {
    static const std::string s = std::string(actorPreviewShaderSource()) + R"(
// ---- the MATERIAL path: the same AverVertex/AverSurface contract PbrShaders.cpp declares, so the
// graph editor can put a .ocgraph's own averEvalMaterial on this sphere ----
//
// Shaded with the EXACT SAME key light and hemisphere fill PreviewPS uses above -- not the material
// system's own BRDF (averShadeDirect/averShadeIndirect), which PbrShaders.cpp's own tests already
// exercise end to end. The only thing this entry point changes relative to PreviewPS is WHERE the
// surface colour comes from: gBaseColor there, the graph's own averEvalMaterial here. Sharing the
// lighting model is what keeps the two previews COMPARABLE side by side, rather than one reading
// brighter or flatter because it took a different shading path.
float4 PreviewMaterialPS(PreviewOut i) : SV_TARGET {
    AverVertex v;
    v.wpos = i.wpos;
    v.N    = normalize(i.nrmWS);
    v.V    = normalize(gPreviewEye.xyz - i.wpos);
    // Two-sided, exactly like averVertexOf's own comment in PbrShaders.cpp: a closed preview sphere
    // never needs this, but a future flat preview mesh (a plane, say) should not shade black on the
    // half of it facing away from the light.
    v.backFace = dot(v.N, v.V) < 0.0;
    if (v.backFace) v.N = -v.N;
    v.uv = i.uv;

    // averBuildSurface reads l.direction alone, to build the half vector H. radiance and visibility
    // exist for averShadeDirect/averShadeIndirect, neither of which this entry point calls, so there
    // is nothing honest to compute for them here -- they are left at the identity rather than wired
    // to a light this shader shades with its own formula, not the BRDF's.
    AverLight l;
    l.direction  = normalize(gPreviewKey.xyz);
    l.radiance   = float3(0.0, 0.0, 0.0);
    l.visibility = 1.0;

    AverSurface s = averEvalMaterial(v, l);

    float ndl = saturate(dot(s.N, l.direction));
    float up = s.N.z * 0.5 + 0.5;
    float3 fill = lerp(gPreviewAmbient.rgb * 0.35, gPreviewAmbient.rgb, up);
    float3 lit = s.albedo * (fill + ndl * gPreviewKey.w);

    float rim = pow(1.0 - saturate(dot(s.N, v.V)), 3.0);
    lit += gPreviewAmbient.w * rim * float3(1.0, 0.62, 0.2);

    return float4(toGamma(acesTonemap(lit)), 1.0);
}
)";
    return s.c_str();
}
#endif

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
    // ADDED FOR THE MATERIAL PATH: AverVertex (PbrShaders.cpp) carries a uv, and a graph that
    // samples a map needs one to sample it with. PreviewPS below still ignores it -- the simple
    // shader has no texture to sample -- so this costs it one unread interpolant, not a behaviour
    // change.
    float2 uv    : TEXCOORD1;
};

// Transforms a vertex to clip space and its normal to world space.
PreviewOut PreviewVS(VSIn i) {
    PreviewOut o;
    float4 wp = mul(float4(i.pos, 1.0), gWorld);
    o.wpos  = wp.xyz;
    o.pos   = mul(wp, gPreviewViewProj);
    o.nrmWS = normalize(averTransformNormal(i.nrm, gWorld));
    // Taken straight from VSIn, exactly like VSMain does in RHIShaders.cpp -- VSIn already carries
    // it (every mesh in this engine does), so nothing upstream of this shader has to change.
    o.uv    = i.uv;
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
#if AVER_MODULE_PBR
    if (materialPipeline_) res_->destroyPipeline(materialPipeline_);
    if (materialVs_) res_->destroyShader(materialVs_);
    if (materialPs_) res_->destroyShader(materialPs_);
    materialFallback_.shutdown();
#endif
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

#if AVER_MODULE_PBR
// (Re)builds the material pipeline against pbr::materialGraphs() as it stands right now. See the
// declaration's own comment for when this is called and why a failure keeps the old pipeline.
bool ActorPreview::createMaterialPipeline() {
    if (!res_) return false;

    if (!materialFallback_.ready()) {
        // Table 0 is empty on this feature -- it declares no other SRVs -- so the material's eight
        // textures land at t0 in TABLE 1 (srvCount1 below), which is the table setDrawBinding always
        // targets (see RHIResources.hpp's own comment on it). tableBaseRegister is therefore 0, the
        // same "the consuming pipeline's table-0 SRV count" MaterialSystem::init() asks for.
        if (!materialFallback_.init(*device_, 0)) {
            AVER_ERROR("[Preview] the material fallback textures could not be created");
            return false;
        }
    }

    // materialShaderDefines()'s tableBaseRegister and sampler register must be the SAME two numbers
    // the layout below declares, or the shader samples registers the root signature never bound.
    // Stored on the instance (not a local): ShaderDesc::defines is a raw pointer, read by the
    // backend -- and, in ActorPreviewTest, recorded and read back later -- after this function
    // returns.
    // NO COAT IN THE PREVIEW, and it is a stated limitation rather than an oversight. This module
    // links Aver.Core and the RHI, not Aver.Render.Voxi, so it cannot see voxi::Settings to know
    // whether the project asked for a layered BSDF -- and reaching for that dependency to light one
    // preview sphere would couple the material-graph editor to the scene renderer. The consequence,
    // said plainly: a coated material previews WITHOUT its coat. Wiring it means giving this module
    // a way to be told the setting, not a way to go and read it.
    materialDefines_ = pbr::materialShaderDefines(/*tableBaseRegister=*/0, /*samplerRegister=*/0,
                                                  /*layeredBsdf=*/false);

    rhi::ShaderDesc vd;
    vd.source = actorPreviewShaderSource();
    vd.prelude = actorPreviewMaterialPrelude();
    vd.entry = "PreviewVS";
    vd.stage = rhi::ShaderStage::Vertex;
    vd.defines = materialDefines_.c_str();
    const rhi::ShaderHandle vs = res_->createShader(vd);

    rhi::ShaderDesc pd = vd;
    pd.source = actorPreviewMaterialShaderSource();
    pd.entry = "PreviewMaterialPS";
    pd.stage = rhi::ShaderStage::Pixel;
    const rhi::ShaderHandle ps = res_->createShader(pd);

    if (!vs || !ps) {
        AVER_WARN("[Preview] the material shaders would not compile; keeping the last good pipeline");
        if (vs) res_->destroyShader(vs);
        if (ps) res_->destroyShader(ps);
        return false;
    }

    rhi::GraphicsPipelineDesc gp;
    gp.vs = vs;
    gp.ps = ps;
    gp.cull = rhi::CullMode::Back;
    gp.depth = {true, true, rhi::CompareOp::Less};
    gp.renderTargetCount = 1;
    gp.renderTargets[0] = rhi::Format::RGBA8Unorm;
    gp.depthFormat = rhi::Format::D32Float;
    gp.sampleCount = 1;
    gp.layout.constantDwords[rhi::kObjectConstantRegister] = rhi::kObjectConstantDwords;
    gp.layout.constantDwords[rhi::kFeatureFrameConstantRegister] = 0;   // a root CBV, same as pipeline_
    // The material system's eight textures (TABLE 1, so setDrawBinding reaches them) and the one
    // sampler they all read through. Every one of the eight is an ordinary Texture2D -- SlotKind's
    // own default -- so unlike Voxi's giLayout() there is nothing non-default to declare, and
    // slotKindsDeclared is left false: the backend reflects the kinds out of the shader, which is
    // exactly correct here because averStockAuthored calls averSampleMaps unconditionally, so every
    // declared slot really is used by the shader reflection would see.
    gp.layout.srvCount1 = pbr::kMaterialSrvCount;
    gp.layout.samplerCount = 1;
    gp.layout.samplers[0].filter = rhi::Filter::Anisotropic;
    gp.layout.samplers[0].address = rhi::AddressMode::Wrap;
    gp.layout.samplers[0].maxAnisotropy = 8;

    const rhi::PipelineHandle pipe = res_->createGraphicsPipeline(gp);
    if (!pipe) {
        AVER_WARN("[Preview] the material pipeline would not build; keeping the last good one");
        res_->destroyShader(vs);
        res_->destroyShader(ps);
        return false;
    }

    // Torn down only now that the replacement fully exists -- see the declaration's comment on why
    // a failure above this point must leave whatever was already live untouched.
    if (materialPipeline_) res_->destroyPipeline(materialPipeline_);
    if (materialVs_) res_->destroyShader(materialVs_);
    if (materialPs_) res_->destroyShader(materialPs_);
    materialPipeline_ = pipe;
    materialVs_ = vs;
    materialPs_ = ps;
    materialGraphRev_ = pbr::materialGraphs().revision();
    AVER_INFO("[Preview] material pipeline rebuilt for {} graph(s), pipeline {}",
              pbr::materialGraphs().count(), materialPipeline_);
    return true;
}
#endif

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

#if AVER_MODULE_PBR
    // A MATERIAL GRAPH THAT APPEARED (OR CHANGED) SINCE materialPipeline_ WAS LAST BUILT. Pulled
    // here rather than pushed from wherever a graph is authored, for the identical reason
    // VoxiRenderer::prePass pulls the same revision number: materials load into the process-wide
    // registry long after this feature is constructed, and a revision check that only lives in ONE
    // renderer's prePass cannot be forgotten by a future second one. Costs nothing when no graph has
    // ever been registered -- materialGraphs().count() is the short-circuit, so the common case (no
    // graphs at all) never even reads the revision.
    if (pbr::materialGraphs().count() > 0 && materialGraphRev_ != pbr::materialGraphs().revision()) {
        if (!createMaterialPipeline())
            AVER_WARN("[Preview] material pipeline rebuild declined; graph-shaded draws keep using "
                     "whatever compiled last, or the simple shader if nothing ever has");
    }
#endif

    ctx.pushMarker("ActorPreview");

    ctx.textureBarrier(color_, everRendered_ ? rhi::ResourceState::ShaderResource
                                             : rhi::ResourceState::ShaderResource,
                       rhi::ResourceState::RenderTarget);

    const rhi::TextureHandle targets[1] = {color_};
    ctx.setRenderTargets(targets, 1, depth_);
    ctx.setViewport(0, 0, width_, height_);
    ctx.setScissor(0, 0, width_, height_);
    ctx.clearDepth(depth_, 1.0f);
    // THE COLOUR TARGET WAS NEVER CLEARED. Only depth was, so whatever the mesh did not cover kept
    // every previous frame's pixels -- an actor with holes, or one that got smaller as its scale was
    // dragged down, composited its own trail forever instead of showing background there. Same
    // chrome grey as the level viewport (SandboxApp.cpp:1136), since this is the same kind of
    // editor surface.
    const f32 kChromeGrey[4] = {0.055f, 0.055f, 0.062f, 1.0f};
    ctx.clearColor(color_, kChromeGrey);

    ctx.setPipeline(pipeline_);
    rhi::PipelineHandle activePipeline = pipeline_;

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

        // THE PIPELINE IS SELECTED BEFORE ANYTHING IS BOUND TO IT, and the order is not cosmetic.
        //
        // Two pipelines mean two ROOT SIGNATURES, and setting one discards every root argument bound
        // under the other -- so a per-draw constant written before the switch is written into the
        // outgoing signature and simply lost. The camera block at b4 was the casualty: the material
        // sphere was transformed by a view-projection of zeroes and landed nowhere on screen, which
        // presents as "the material pipeline draws nothing" and survived a constant-colour pixel
        // shader, a debug-layer run with no errors at all, and every reading of the shader itself.
        //
        // It could not happen while there was only ONE pipeline, because the switch then ran at most
        // once per frame and the bindings after it were the ones that counted. Adding a second is
        // what made the ordering load-bearing.
        //
        // 0 -- EVERY EXISTING CALLER's value -- selects pipeline_ unchanged. A non-zero id selects
        // the material pipeline only once one actually exists; a graph that has not compiled yet
        // (or a PBR=OFF build, where materialPipeline_ does not exist as a member at all) quietly
        // falls back to the simple shader rather than skipping the draw.
        rhi::PipelineHandle wanted = pipeline_;
#if AVER_MODULE_PBR
        const bool wantsMaterial = d.materialGraphId != 0 && materialPipeline_ != 0;
        if (wantsMaterial) wanted = materialPipeline_;
#endif
        if (wanted != activePipeline) {
            ctx.setPipeline(wanted);
            activePipeline = wanted;
        }

        frame.ambient[3] = d.selected ? 0.9f : 0.0f;
        ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &frame, sizeof(frame));

#if AVER_MODULE_PBR
        if (wantsMaterial) {
            // The identity material's factors, so anything the graph does NOT drive (a roughness
            // map, an occlusion map) reads as the neutral value averStockAuthored would give a
            // material with nothing bound -- with graphId overwritten to select this draw's graph
            // out of the process-wide switch materialGraphHlsl() generated. The fallback BINDING SET
            // supplies the eight identity textures the same call reads maps through, so a graph that
            // samples one gets a defined answer (white, flat, or black, per slot) rather than an
            // unbound descriptor.
            pbr::MaterialConstants mc = materialFallback_.fallbackConstants();
            mc.graphId = d.materialGraphId;
            ctx.setDrawBinding(materialFallback_.fallbackBindingSet(), &mc, sizeof(mc));
        }
#endif

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
