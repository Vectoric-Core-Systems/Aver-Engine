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

Vec3 unprojectPoint(const f32 ivp[16], f32 ndcX, f32 ndcY, f32 ndcZ) {
    const f32 v[4] = {ndcX, ndcY, ndcZ, 1.0f};
    f32 r[4] = {0, 0, 0, 0};
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
            r[j] += v[i] * ivp[i * 4 + j];
    const f32 invW = r[3] != 0.0f ? 1.0f / r[3] : 1.0f;
    return {r[0] * invW, r[1] * invW, r[2] * invW};
}

// COPIED FROM particles::ParticleRenderer's anonymous namespace, credited rather than extracted.
// It is ten lines with no header of its own, and hoisting it into a shared one for a second caller
// would mean a new module dependency from the editor onto Aver.Particles -- or a third module for
// two functions -- to save ten lines. The proof that it is EXACT rather than a finite-difference
// approximation lives with the original; the short version is that holding ndc.z fixed holds
// view-space Z fixed for this engine's perspective matrix, and ndc.x is then linear in view-space X,
// so a finite step in ndc.x maps purely along the camera's right axis at any step size.
void cameraBasis(const f32 invViewProj[16], Vec3& outRight, Vec3& outUp) {
    const Vec3 center = unprojectPoint(invViewProj, 0.0f, 0.0f, 0.5f);
    const Vec3 rightP = unprojectPoint(invViewProj, 0.5f, 0.0f, 0.5f);
    const Vec3 upP    = unprojectPoint(invViewProj, 0.0f, 0.5f, 0.5f);
    outRight = (rightP - center).getSafeNormal();
    outUp    = (upP - center).getSafeNormal();
}

// STRAIGHT alpha, unlike ParticleRenderer's packPremultiplied twin -- viewport_icon.hlsl does the
// premultiply itself, after its inverse tonemap, because doing it before a non-linear curve puts a
// dark fringe on every transparent edge. Byte order R,G,B,A from the low byte up (0xAABBGGRR), the
// same convention UiDrawList and the particle renderer both use for an RGBA8Unorm vertex colour.
u32 packStraight(const f32 rgba[4]) {
    auto b = [](f32 v) -> u32 {
        const f32 c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<u32>(c * 255.0f + 0.5f);
    };
    return b(rgba[0]) | (b(rgba[1]) << 8) | (b(rgba[2]) << 16) | (b(rgba[3]) << 24);
}

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
    // gViewProj from the frame block, and averInverseTonemap/srgbToLin from the prelude's own maths
    // -- see IconPS's comment on why that round trip is not optional here.
    sd.prelude = rhi::sharedShaderPrelude();
    sd.entry = "IconVS";
    sd.stage = rhi::ShaderStage::Vertex;
    vs_ = res_->createShader(sd);
    sd.entry = "IconPS";
    sd.stage = rhi::ShaderStage::Pixel;
    ps_ = res_->createShader(sd);
    if (!vs_ || !ps_) {
        AVER_ERROR("[ViewportIcons] the viewport icon shaders failed to compile");
        return false;
    }

    if (!ensureCapacity(kInitialVertices, kInitialIndices)) return false;

    // Best-effort first build; the real scene target's sample count and formats arrive through
    // onRenderTargetsChanged before the first real frame. Same two-step as ParticleRenderer::init.
    buildPipeline(device.sampleCount(), device.backbufferFormat(), device.depthFormat());
    AVER_INFO("[ViewportIcons] ready");
    return true;
}

void ViewportIconRenderer::shutdown() {
    if (res_) {
        for (u32 i = 0; i < kFramesInFlight; ++i) { res_->destroyBuffer(vb_[i]); res_->destroyBuffer(ib_[i]); }
        for (Icon& ic : icons_) {
            res_->destroyBindingSet(ic.set);
            res_->destroyTexture(ic.texture);
        }
        res_->destroyPipeline(pso_);
        res_->destroyShader(vs_);
        res_->destroyShader(ps_);
    }
    for (u32 i = 0; i < kFramesInFlight; ++i) { vb_[i] = 0; ib_[i] = 0; }
    icons_.assign(1, Icon{});
    pso_ = 0;
    vs_ = ps_ = 0;
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

bool ViewportIconRenderer::buildPipeline(u32 sampleCount, rhi::Format color, rhi::Format depth) {
    if (!res_ || !vs_ || !ps_) return false;
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

    // TESTED, NOT WRITTEN -- the transparentPass contract, and the entire reason this is a render
    // feature rather than an ImGui overlay. Test gives occlusion by real geometry; write-off keeps
    // the icon from occluding anything drawn after it.
    gd.depth.test = true;
    gd.depth.write = false;
    // A billboard faces the camera by construction; there is no back face to cull.
    gd.cull = rhi::CullMode::None;
    gd.blend = rhi::BlendMode::PremultipliedAlpha;

    gd.renderTargetCount = 1;
    gd.renderTargets[0] = color;
    gd.depthFormat = depth;
    gd.sampleCount = sampleCount;

    gd.layout.srvCount = 1;
    gd.layout.samplerCount = 1;
    gd.layout.samplers[0].filter = rhi::Filter::Linear;
    gd.layout.samplers[0].address = rhi::AddressMode::Clamp;

    pso_ = res_->createGraphicsPipeline(gd);
    pipelineSampleCount_ = sampleCount;
    pipelineColor_ = color;
    pipelineDepth_ = depth;
    if (!pso_) {
        AVER_ERROR("[ViewportIcons] pipeline build failed at {}x MSAA", sampleCount);
        return false;
    }
    AVER_INFO("[ViewportIcons] pipeline (re)built: {}x MSAA", sampleCount);
    return true;
}

void ViewportIconRenderer::onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth,
                                                  u32 width, u32 height) {
    (void)width; (void)height;
    if (sampleCount == pipelineSampleCount_ && color == pipelineColor_ && depth == pipelineDepth_) return;
    buildPipeline(sampleCount, color, depth);
}

void ViewportIconRenderer::transparentPass(rhi::IRenderContext& ctx) {
    // CLEARED EVEN ON EVERY EARLY RETURN BELOW, which is what makes addIcon safe to call from a
    // frame that never reaches a draw. Leave the queue standing and a run with no pipeline would
    // accumulate one request per icon per frame until it ran out of memory.
    struct ClearOnExit {
        std::vector<Request>* q;
        ~ClearOnExit() { q->clear(); }
    } clearer{&pending_};

    if (pending_.empty() || !pso_ || !res_ || !dev_) return;

    f32 viewProj[16], invViewProj[16], camPos[3];
    if (!dev_->camera(viewProj, invViewProj, camPos)) return;

    Vec3 right, up;
    cameraBasis(invViewProj, right, up);
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
            // and the icon renders upside down, which for a map pin is unmistakable.
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

    // Viewport, scissor and both scene targets are already bound -- the transparentPass contract --
    // so only the pipeline and our own buffers are set here.
    ctx.pushMarker("Aver.ViewportIcons");
    ctx.setPipeline(pso_);
    ctx.setVertexBuffer(vb, sizeof(ViewportIconVertex));
    ctx.setIndexBuffer(ib, rhi::Format::R32Uint);
    for (const DrawCmd& d : draws_) {
        const Icon& ic = icons_[d.icon];
        if (!ic.set) continue;
        ctx.setBindingSet(ic.set, 0);
        ctx.drawIndexed(d.indexCount, d.indexOffset, 0);
    }
    ctx.popMarker();
}

} // namespace aver::editor
