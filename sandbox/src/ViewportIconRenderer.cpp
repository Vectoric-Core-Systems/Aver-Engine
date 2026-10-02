#include "ViewportIconRenderer.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/Image.hpp"
#include "aver/rhi/ShaderFiles.hpp"   // this pass's HLSL is a deployed file, not a literal

#include <algorithm>
#include <cstddef>

namespace aver::editor {
namespace {

constexpr usize kInitialVertices = 64;   // 16 icons before the first grow; the editor draws a handful
constexpr usize kInitialIndices  = 96;

// Through the camera-relative inverse (rhi::PerFrameCB::invViewProjRel): the result is an offset
// FROM THE EYE; an absolute point needs camPos added.
Vec3 unprojectPoint(const f32 ivpRel[16], f32 ndcX, f32 ndcY, f32 ndcZ) {
    const f32 v[4] = {ndcX, ndcY, ndcZ, 1.0f};
    f32 r[4] = {0, 0, 0, 0};
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
            r[j] += v[i] * ivpRel[i * 4 + j];
    const f32 invW = r[3] != 0.0f ? 1.0f / r[3] : 1.0f;
    return {r[0] * invW, r[1] * invW, r[2] * invW};
}

// COPIED FROM particles::ParticleRenderer's anonymous namespace, credited rather than extracted.
// It is ten lines with no header of its own, and hoisting it into a shared one for a second caller
// would mean a new module dependency from the editor onto Aver.Particles -- or a third module for
// two functions -- to save ten lines. The proof that it is EXACT rather than a finite-difference
// approximation lives with the original; the short version is that holding ndc.z fixed holds
// view-space Z fixed for this engine's perspective matrix, and ndc.x is then linear in view-space X,
// so a finite step in ndc.x maps purely along the camera's right axis at any step size. The points
// are camera-relative, centimetres from the eye rather than at its world magnitude, so their
// differences stay exact far from the origin.
void cameraBasis(const f32 invViewProjRel[16], Vec3& outRight, Vec3& outUp) {
    const Vec3 center = unprojectPoint(invViewProjRel, 0.0f, 0.0f, 0.5f);
    const Vec3 rightP = unprojectPoint(invViewProjRel, 0.5f, 0.0f, 0.5f);
    const Vec3 upP    = unprojectPoint(invViewProjRel, 0.0f, 0.5f, 0.5f);
    outRight = (rightP - center).getSafeNormal();
    outUp    = (upP - center).getSafeNormal();
}

// STRAIGHT alpha, unlike ParticleRenderer's packPremultiplied twin -- viewport_icon.hlsl does the
// premultiply itself, after the optional sRGB-to-linear step (IconPS), because doing it before a
// non-linear curve puts a dark fringe on every transparent edge. Byte order R,G,B,A from the low
// byte up (0xAABBGGRR), the same convention UiDrawList and the particle renderer both use for an
// RGBA8Unorm vertex colour.
u32 packStraight(const f32 rgba[4]) {
    auto b = [](f32 v) -> u32 {
        const f32 c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<u32>(c * 255.0f + 0.5f);
    };
    return b(rgba[0]) | (b(rgba[1]) << 8) | (b(rgba[2]) << 16) | (b(rgba[3]) << 24);
}

// The overlay/display target's REAL colour format -- NOT IDevice::backbufferFormat(), which (despite
// its name) returns the SCENE colour target's own HDR format (RGBA16F): see that method's own
// comment in RHI.hpp, and D3D12Device.cpp's own overlayPass call site ("NOT IDevice::backbufferFormat
// (): here that names the SCENE colour target's own format ... the real backbuffer and the viewport
// texture are both created at kBackbufferFormat"). Both shipping backends' actual swapchain surface is
// an 8-bit UNORM format (D3D12Device.cpp's kBackbufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
// VulkanDevice.cpp negotiates VK_FORMAT_R8G8B8A8_UNORM or VK_FORMAT_B8G8R8A8_UNORM, both
// Format::RGBA8Unorm here) -- there is no generic IDevice accessor for it today (EditorLines is
// handed it directly by the device instead of asking, since it isn't a generic IRenderFeature), so
// this is hardcoded rather than queried.
constexpr rhi::Format kOverlayTargetFormat = rhi::Format::RGBA8Unorm;

// sRGB render-target formats decode on read and encode on write; IconPS needs to know so it can
// write LINEAR when the hardware is about to re-encode, the same convention editor_lines.hlsl's
// gMaterial.x follows. Always false against kOverlayTargetFormat today (a plain UNORM backbuffer --
// the composite shader gamma-encodes by hand, per docs/STATUS.md's "ACES -> gamma -> backbuffer"),
// kept dynamic rather than assumed for the same reason editor_lines.hlsl's replay() checks its own
// targetFormat argument instead of assuming: it costs nothing and stays right if that ever changes.
bool isSrgbTargetFormat(rhi::Format f) { return f == rhi::Format::RGBA8UnormSrgb; }

// Mirrors shared_prelude.hlsl's PerObject cbuffer field-for-field, so the fields below can be named
// rather than poked in as raw dword offsets. gWorld is left at its zero default -- IconVS never
// reads it -- and gShadingModel/gReflectance/gF90/_objPad are unread by IconPS, same as
// editor_lines.hlsl leaves them.
struct ObjectConsts {
    f32 world[16] = {};
    f32 baseColor[4] = {};   // .y = depth-tested, .zw = the 3D view's rect size (px)
    f32 material[4] = {};    // .x = target is sRGB (write linear), .zw = the rect's origin (px)
    u32 shadingModel = 0;
    f32 reflectance = 0, f90 = 0, _objPad = 0;
    f32 emissive[4] = {};    // .xy = the whole (display) target's size (px)
};
static_assert(sizeof(ObjectConsts) == rhi::kObjectConstantDwords * sizeof(f32),
              "must match PerObject's 32-dword layout (shared_prelude.hlsl)");

} // namespace

bool ViewportIconRenderer::init(rhi::IDevice& device) {
    dev_ = &device;
    res_ = device.resources();
    if (!res_) {
        AVER_WARN("[ViewportIcons] init declined: backend exposes no resource factory");
        return false;
    }

    rhi::ShaderDesc sd;
    sd.source = rhi::shaderFile("viewport_icon.hlsl").c_str();
    sd.prelude = rhi::sharedShaderPrelude();
    sd.entry = "IconVS";
    sd.stage = rhi::ShaderStage::Vertex;
    vs_ = res_->createShader(sd);
    if (!vs_) {
        AVER_ERROR("[ViewportIcons] IconVS failed to compile");
        return false;
    }

    if (!ensureCapacity(kInitialVertices, kInitialIndices)) return false;

    // Best-effort first build; the real scene depth sample count arrives through
    // onRenderTargetsChanged before the first real frame, same two-step as ParticleRenderer::init.
    // IconPS itself is built here, inside ensurePipeline.
    ensurePipeline(device.sampleCount());
    AVER_INFO("[ViewportIcons] ready");
    return true;
}

void ViewportIconRenderer::shutdown() {
    if (res_) {
        for (u32 i = 0; i < kFramesInFlight; ++i) {
            res_->destroyBuffer(vb_[i]); res_->destroyBuffer(ib_[i]);
            res_->destroyBindingSet(depthSet_[i]);
        }
        for (Icon& ic : icons_) {
            res_->destroyBindingSet(ic.set);
            res_->destroyTexture(ic.texture);
        }
        res_->destroyPipeline(pso_);
        res_->destroyShader(vs_);
        res_->destroyShader(ps_);
    }
    for (u32 i = 0; i < kFramesInFlight; ++i) { vb_[i] = 0; ib_[i] = 0; depthSet_[i] = 0; }
    icons_.assign(1, Icon{});
    pso_ = 0;
    vs_ = ps_ = 0;
    builtDepthSamples_ = 0;
    vbCapacity_ = ibCapacity_ = 0;
    res_ = nullptr;
    dev_ = nullptr;
}

ViewportIconRenderer::IconHandle ViewportIconRenderer::loadIcon(const std::string& pngPath,
                                                                const char* debugName) {
    if (!res_) return kNoIcon;

    ImageData img;
    std::string why;
    if (!decodeImage(pngPath, img, &why)) {
        AVER_WARN("[ViewportIcons] '{}' not loaded ({}) -- that marker draws nothing", pngPath, why);
        return kNoIcon;
    }

    rhi::TextureDesc td;
    td.width = img.width;
    td.height = img.height;
    td.format = rhi::Format::RGBA8Unorm;
    td.bind = rhi::ResourceBind::ShaderResource;
    td.initialState = rhi::ResourceState::ShaderResource;
    td.debugName = debugName;
    const void* levels[1] = {img.pixels.data()};
    td.initialData = levels;
    td.initialDataCount = 1;
    td.initialRowPitch = img.rowPitch();
    const rhi::TextureHandle tex = res_->createTexture(td);
    if (!tex) {
        AVER_WARN("[ViewportIcons] '{}' could not be uploaded", pngPath);
        return kNoIcon;
    }

    // ONE BINDING SET PER ICON, not one shared set re-pointed per draw. setSrv writes a descriptor
    // that the GPU reads when the draw actually executes, not when the call is made, so rewriting a
    // single set between two draws in the same command list would give BOTH draws whichever texture
    // was written last. A set per icon costs one descriptor each and removes that entirely.
    rhi::BindingSetDesc bsd;
    bsd.srvCount = 1;
    bsd.srvKinds[0] = rhi::SlotKind::Texture2D;
    bsd.srvBaseRegister = 0;
    const rhi::BindingSetHandle set = res_->createBindingSet(bsd);
    if (!set) {
        AVER_WARN("[ViewportIcons] '{}' has no binding set", pngPath);
        res_->destroyTexture(tex);
        return kNoIcon;
    }
    res_->setSrv(set, 0, tex);

    icons_.push_back(Icon{tex, set});
    AVER_INFO("[ViewportIcons] {} decoded from {} ({}x{})", debugName, pngPath, img.width, img.height);
    return static_cast<IconHandle>(icons_.size() - 1);
}

void ViewportIconRenderer::addIcon(const Vec3& worldPos, f32 halfSize, IconHandle icon, f32 alpha) {
    if (icon == kNoIcon || icon >= icons_.size()) return;
    if (halfSize <= 0.0f || alpha <= 0.0f) return;
    pending_.push_back(Request{worldPos, halfSize, icon, alpha});
}

bool ViewportIconRenderer::ensureCapacity(usize vertexCount, usize indexCount) {
    const bool growV = vertexCount > vbCapacity_;
    const bool growI = indexCount > ibCapacity_;
    if (!growV && !growI) return true;

    const usize newV = growV ? vertexCount : vbCapacity_;
    const usize newI = growI ? indexCount : ibCapacity_;
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        if (growV) {
            res_->destroyBuffer(vb_[i]);
            rhi::BufferDesc bd;
            bd.bytes = static_cast<u64>(newV) * sizeof(ViewportIconVertex);
            bd.kind = rhi::BufferKind::Upload;
            bd.debugName = "Aver.ViewportIcons vertices";
            vb_[i] = res_->createBuffer(bd);
            if (!vb_[i]) return false;
        }
        if (growI) {
            res_->destroyBuffer(ib_[i]);
            rhi::BufferDesc bd;
            bd.bytes = static_cast<u64>(newI) * sizeof(u32);
            bd.kind = rhi::BufferKind::Upload;
            bd.debugName = "Aver.ViewportIcons indices";
            ib_[i] = res_->createBuffer(bd);
            if (!ib_[i]) return false;
        }
    }
    vbCapacity_ = newV;
    ibCapacity_ = newI;
    return true;
}

bool ViewportIconRenderer::ensurePipeline(u32 depthSamples) {
    if (pso_ && depthSamples == builtDepthSamples_) return true;
    if (!res_ || !vs_) return false;

    // The depth read's dimensionality is a compile-time choice (Texture2D vs Texture2DMS, matching
    // editor_lines.hlsl's AVER_EDITOR_LINE_MS) and a binding-set-declared one (SlotKind below), so
    // IconPS is recompiled here rather than kept as a permanent MS/non-MS pair -- OcclusionCuller's
    // HZB seed kernel is the precedent (AVER_HZB_MS), and a sample-count change is rare enough that
    // recompiling on it costs nothing worth avoiding.
    const bool ms = depthSamples > 1;
    res_->destroyShader(ps_);
    rhi::ShaderDesc sd;
    sd.source = rhi::shaderFile("viewport_icon.hlsl").c_str();
    sd.prelude = rhi::sharedShaderPrelude();
    sd.entry = "IconPS";
    sd.stage = rhi::ShaderStage::Pixel;
    sd.defines = ms ? "AVER_ICON_DEPTH_MS=1" : nullptr;
    ps_ = res_->createShader(sd);
    if (!ps_) {
        AVER_ERROR("[ViewportIcons] IconPS failed to (re)compile ({} depth sample(s))", depthSamples);
        return false;
    }

    res_->destroyPipeline(pso_);
    pso_ = 0;

    rhi::GraphicsPipelineDesc gd;
    gd.vs = vs_;
    gd.ps = ps_;

    gd.vertexLayout.stride = sizeof(ViewportIconVertex);
    gd.vertexLayout.attribCount = 4;
    gd.vertexLayout.attribs[0] = {rhi::VertexSemantic::Position, 0, rhi::Format::RG32Float, 0};
    gd.vertexLayout.attribs[1] = {rhi::VertexSemantic::Position, 1, rhi::Format::R32Float, 8};
    gd.vertexLayout.attribs[2] = {rhi::VertexSemantic::TexCoord, 0, rhi::Format::RG32Float, 12};
    gd.vertexLayout.attribs[3] = {rhi::VertexSemantic::Color, 0, rhi::Format::RGBA8Unorm, 20};
    static_assert(offsetof(ViewportIconVertex, uv) == 12, "TEXCOORD0 is declared at byte 12 above");
    static_assert(offsetof(ViewportIconVertex, color) == 20, "COLOR0 is declared at byte 20 above");

    // NEITHER TESTED NOR WRITTEN, unlike the old transparentPass pipeline: overlayPass has no depth
    // attachment at all (it draws onto the display target, after the scene and its depth buffer are
    // both finished with). IconPS does its own occlusion instead, sampling IDevice::sceneDepthTexture
    // through the table-1 binding below and comparing distances from the eye -- PSEditorLine's own
    // technique (editor_lines.hlsl), needed there for the identical reason (a render-scaled scene
    // depth doesn't line up 1:1 with the display target a hardware depth-test would need).
    gd.depth.test = false;
    gd.depth.write = false;
    // A billboard faces the camera by construction; there is no back face to cull.
    gd.cull = rhi::CullMode::None;
    gd.blend = rhi::BlendMode::PremultipliedAlpha;

    gd.renderTargetCount = 1;
    gd.renderTargets[0] = kOverlayTargetFormat;
    gd.depthFormat = rhi::Format::Unknown;
    // The OVERLAY target itself is never multisampled (it is the display target); `depthSamples`
    // above is the SCENE depth's sample count, a completely different axis (what table 1 reads).
    gd.sampleCount = 1;

    gd.layout.srvCount = 1;    // table 0: t0, this icon's own texture (per-icon set, unchanged)
    gd.layout.srvCount1 = 1;   // table 1: t1, the sampled scene depth (depthSet_ ring, below)
    gd.layout.slotKindsDeclared = true;
    gd.layout.srvKinds[0] = rhi::SlotKind::Texture2D;
    gd.layout.srvKinds1[0] = ms ? rhi::SlotKind::Texture2DMS : rhi::SlotKind::Texture2D;
    gd.layout.samplerCount = 1;
    gd.layout.samplers[0].filter = rhi::Filter::Linear;
    gd.layout.samplers[0].address = rhi::AddressMode::Clamp;
    // b1 as root constants, DECLARED: undeclared it is a root CBV on D3D12 and overlayPass's
    // setConstants is refused (same as ActorPreview's object block).
    gd.layout.constantDwords[rhi::kObjectConstantRegister] = rhi::kObjectConstantDwords;

    pso_ = res_->createGraphicsPipeline(gd);
    if (!pso_) {
        AVER_ERROR("[ViewportIcons] overlay pipeline build failed ({} depth sample(s))", depthSamples);
        return false;
    }

    // Table 1's binding sets share the pipeline's MS-ness, so they are rebuilt alongside it --
    // destroyed and recreated on change rather than kept as a permanent pair, same call as IconPS
    // above. The SRV itself (the actual depth texture) is written per frame in overlayPass, since the
    // resource can be recreated by a resize independently of this shape decision.
    for (u32 i = 0; i < kFramesInFlight; ++i) res_->destroyBindingSet(depthSet_[i]);
    rhi::BindingSetDesc dd;
    dd.srvCount = 1;
    dd.srvKinds[0] = ms ? rhi::SlotKind::Texture2DMS : rhi::SlotKind::Texture2D;
    dd.srvBaseRegister = 1;
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        depthSet_[i] = res_->createBindingSet(dd);
        if (!depthSet_[i]) {
            AVER_ERROR("[ViewportIcons] depth binding set {} unavailable", i);
            return false;
        }
    }

    builtDepthSamples_ = depthSamples;
    AVER_INFO("[ViewportIcons] overlay pipeline (re)built: {} depth sample(s)", depthSamples);
    return true;
}

void ViewportIconRenderer::onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth,
                                                  u32 width, u32 height) {
    // `sampleCount` is the SCENE target's sample count (this call's own contract) -- what decides
    // Texture2D vs Texture2DMS for the depth read below. `color`/`depth` are the SCENE target's
    // formats, which never mattered for this pipeline (see kOverlayTargetFormat's own comment).
    (void)color; (void)depth; (void)width; (void)height;
    ensurePipeline(sampleCount);
}

void ViewportIconRenderer::overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) {
    // CLEARED EVEN ON EVERY EARLY RETURN BELOW, which is what makes addIcon safe to call from a
    // frame that never reaches a draw. Leave the queue standing and a run with no pipeline would
    // accumulate one request per icon per frame until it ran out of memory.
    struct ClearOnExit {
        std::vector<Request>* q;
        ~ClearOnExit() { q->clear(); }
    } clearer{&pending_};

    if (pending_.empty() || !res_ || !dev_) return;
    if (!ensurePipeline(dev_->sampleCount()) || !pso_) return;

    f32 viewProj[16], invViewProjRel[16], camPos[3];
    if (!dev_->camera(viewProj, invViewProjRel, camPos)) return;

    Vec3 right, up;
    cameraBasis(invViewProjRel, right, up);
    if (right.sizeSquared() < 0.5f || up.sizeSquared() < 0.5f) return;   // a degenerate camera

    // Grouped by icon so each texture is one draw. Stable, so two markers with the same icon keep
    // the order they were added in -- there is no depth sort here because these do not overlap each
    // other in practice and a wrong answer between two icons is invisible next to a wrong answer
    // against the scene, which the depth test already gets right.
    std::stable_sort(pending_.begin(), pending_.end(),
                     [](const Request& a, const Request& b) { return a.icon < b.icon; });

    verts_.clear();
    idx_.clear();
    draws_.clear();

    for (usize i = 0; i < pending_.size();) {
        const IconHandle icon = pending_[i].icon;
        const u32 indexOffset = static_cast<u32>(idx_.size());
        for (; i < pending_.size() && pending_[i].icon == icon; ++i) {
            const Request& r = pending_[i];
            const Vec3 rW = right * r.halfSize, uW = up * r.halfSize;
            const Vec3 corners[4] = {
                r.pos - rW - uW, r.pos + rW - uW,
                r.pos + rW + uW, r.pos - rW + uW,
            };
            // V FLIPPED AGAINST THE CORNER ORDER. The corners run bottom-left, bottom-right,
            // top-right, top-left in WORLD space (up is +Y of the camera basis), while a decoded PNG
            // is stored top row first -- so v=1 belongs to the bottom corners. Get this backwards
            // and the icon renders upside down, which is unmistakable for any of these.
            const f32 uvs[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
            const f32 tint[4] = {1.0f, 1.0f, 1.0f, r.alpha};
            const u32 packed = packStraight(tint);
            const u32 base = static_cast<u32>(verts_.size());
            for (int c = 0; c < 4; ++c) {
                ViewportIconVertex v;
                v.pos[0] = corners[c].x; v.pos[1] = corners[c].y; v.pos[2] = corners[c].z;
                v.uv[0] = uvs[c][0]; v.uv[1] = uvs[c][1];
                v.color = packed;
                verts_.push_back(v);
            }
            const u32 quad[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
            idx_.insert(idx_.end(), quad, quad + 6);
        }
        draws_.push_back(DrawCmd{indexOffset, static_cast<u32>(idx_.size()) - indexOffset, icon});
    }

    if (draws_.empty()) return;
    if (!ensureCapacity(verts_.size(), idx_.size())) return;

    frame_ = (frame_ + 1) % kFramesInFlight;
    const rhi::BufferHandle vb = vb_[frame_], ib = ib_[frame_];
    if (!vb || !ib) return;
    if (!res_->writeBuffer(vb, verts_.data(), verts_.size() * sizeof(ViewportIconVertex))) return;
    if (!res_->writeBuffer(ib, idx_.data(), idx_.size() * sizeof(u32))) return;

    // The scene depth, into THIS frame's own ring slot (see depthSet_'s own comment for why a ring
    // and not one set rewritten in place) -- the same clearSrv/setSrv split EditorLines::replay uses
    // for the identical binding.
    const rhi::TextureHandle sceneDepth = dev_->sceneDepthTexture();
    if (sceneDepth) res_->setSrv(depthSet_[frame_], 0, sceneDepth);
    else res_->clearSrv(depthSet_[frame_], 0);

    // overlayPass hands over the whole display target's viewport/scissor (IRenderFeature::
    // overlayPass's own contract); the icons must be clipped to the 3D VIEW's own rect within it, in
    // DISPLAY pixels -- sceneViewport() reports SCENE-space pixels, which differ from display pixels
    // under a render scale (IDevice::renderScale), the identical conversion EditorLines::replay makes
    // for its own sceneRect argument.
    f32 rect[4];
    if (dev_->sceneViewport(rect)) {
        const f32 scale = dev_->renderScale() > 0.0f ? dev_->renderScale() : 1.0f;
        rect[0] /= scale; rect[1] /= scale; rect[2] /= scale; rect[3] /= scale;
    } else {
        rect[0] = 0.0f; rect[1] = 0.0f; rect[2] = static_cast<f32>(width); rect[3] = static_cast<f32>(height);
    }
    // Clamped into the target, the same way EditorLines::replay clamps its own sceneRect argument --
    // a transient resize can hand either of us a rect that briefly overruns the target it's meant to
    // sit inside, and a negative-size viewport/scissor is a harder failure than a clipped one.
    if (rect[0] < 0.0f) rect[0] = 0.0f;
    if (rect[1] < 0.0f) rect[1] = 0.0f;
    if (rect[0] > static_cast<f32>(width))  rect[0] = static_cast<f32>(width);
    if (rect[1] > static_cast<f32>(height)) rect[1] = static_cast<f32>(height);
    if (rect[2] < 0.0f) rect[2] = 0.0f;
    if (rect[3] < 0.0f) rect[3] = 0.0f;
    if (rect[0] + rect[2] > static_cast<f32>(width))  rect[2] = static_cast<f32>(width)  - rect[0];
    if (rect[1] + rect[3] > static_cast<f32>(height)) rect[3] = static_cast<f32>(height) - rect[1];
    ctx.setViewport(static_cast<u32>(rect[0]), static_cast<u32>(rect[1]),
                    static_cast<u32>(rect[2]), static_cast<u32>(rect[3]));
    ctx.setScissor(static_cast<u32>(rect[0]), static_cast<u32>(rect[1]),
                   static_cast<u32>(rect[2]), static_cast<u32>(rect[3]));

    // setPipeline PRECEDES setConstants/setBindingSet (IRenderContext::setPipeline's own contract) --
    // bound first, before either.
    ctx.pushMarker("Aver.ViewportIcons");
    ctx.setPipeline(pso_);
    ctx.setVertexBuffer(vb, sizeof(ViewportIconVertex));
    ctx.setIndexBuffer(ib, rhi::Format::R32Uint);
    ctx.setBindingSet(depthSet_[frame_], 1);

    // PerObject, filled the way editor_lines.hlsl's replay() fills it for the SAME depth test and
    // sRGB question (see IconPS): gWorld is left zero -- IconVS never reads it, the corners already
    // arrive in world space from the CPU billboard above.
    ObjectConsts oc{};
    oc.baseColor[1] = sceneDepth ? 1.0f : 0.0f;                          // depth-tested
    oc.baseColor[2] = rect[2]; oc.baseColor[3] = rect[3];                // the 3D view's rect size
    oc.material[0] = isSrgbTargetFormat(kOverlayTargetFormat) ? 1.0f : 0.0f;   // target stores sRGB itself
    oc.material[2] = rect[0]; oc.material[3] = rect[1];                 // the 3D view's rect origin
    oc.emissive[0] = static_cast<f32>(width); oc.emissive[1] = static_cast<f32>(height);   // whole target size
    ctx.setConstants(rhi::kObjectConstantRegister, &oc, rhi::kObjectConstantDwords);

    for (const DrawCmd& d : draws_) {
        const Icon& ic = icons_[d.icon];
        if (!ic.set) continue;
        ctx.setBindingSet(ic.set, 0);
        ctx.drawIndexed(d.indexCount, d.indexOffset, 0);
    }
    ctx.popMarker();
}

} // namespace aver::editor
