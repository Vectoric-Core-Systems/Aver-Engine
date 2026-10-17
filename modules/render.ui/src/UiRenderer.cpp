// UiRenderer implementation: the pipeline, the buffer ring and the overlay pass.
#include "aver/render/ui/UiRenderer.hpp"
#include "UiShaders.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include "aver/rhi/ShaderFiles.hpp"   // this pass's HLSL is a deployed file, not a literal

namespace aver::render::ui {
namespace {

constexpr u32 kInitialVertices = 4096;
constexpr u32 kInitialIndices  = 6144;

// The register the shader declares its constants at; see the note in UiShaders.hpp for why b3.
constexpr u32 kUiConstantSlot = 3;

} // namespace

// Builds a renderer on a device. Returns nullptr when the backend cannot host one.
UiRenderer* UiRenderer::create(rhi::IDevice& device) {
    UiRenderer* r = new UiRenderer();
    if (!r->init(device)) { delete r; return nullptr; }
    return r;
}

// Destroys every resource. Destroy of a zero handle is a no-op, so a partial init is safe.
UiRenderer::~UiRenderer() {
    if (!res_) return;
    for (const TexBinding& b : bindings_) res_->destroyBindingSet(b.set);
    for (const PageTexture& p : pages_) res_->destroyTexture(p.tex);
    for (u32 i = 0; i < kFramesInFlight; ++i) { res_->destroyBuffer(vb_[i]); res_->destroyBuffer(ib_[i]); }
    res_->destroyPipeline(pipeline_);
    res_->destroyShader(vs_);
    res_->destroyShader(ps_);
    res_->destroyTexture(white_);
}

// Creates the white texel, the shaders, the pipeline and the first buffers. False declines the UI.
bool UiRenderer::init(rhi::IDevice& device) {
    device_ = &device;
    res_ = device.resources();
    if (!res_) {
        AVER_WARN("[Render.UI] init declined: backend exposes no resource factory");
        return false;
    }

    // ---- the white texel ----
    // An untextured rect is a textured rect sampling this.
    {
        const u32 whitePixel = 0xFFFFFFFFu;
        const void* data = &whitePixel;
        rhi::TextureDesc td;
        td.width = td.height = 1;
        td.format = rhi::Format::RGBA8Unorm;   // NOT sRGB: see the note on colour space in UiShaders
        td.bind = rhi::ResourceBind::ShaderResource;
        td.initialState = rhi::ResourceState::ShaderResource;
        td.initialData = &data;
        td.initialDataCount = 1;
        td.debugName = "Aver.UI white";
        white_ = res_->createTexture(td);
        if (!white_) { AVER_ERROR("[Render.UI] could not create the white texture"); return false; }
    }

    // ---- shaders ----
    {
        rhi::ShaderDesc sd;
        sd.source = rhi::shaderFile("ui.hlsl").c_str();
        sd.entry  = "UiVS";
        sd.stage  = rhi::ShaderStage::Vertex;
        vs_ = res_->createShader(sd);
        sd.entry  = "UiPS";
        sd.stage  = rhi::ShaderStage::Pixel;
        ps_ = res_->createShader(sd);
        if (!vs_ || !ps_) { AVER_ERROR("[Render.UI] UI shaders failed to compile"); return false; }
    }

    // ---- the pipeline ----
    {
        rhi::GraphicsPipelineDesc gd;
        gd.vs = vs_;
        gd.ps = ps_;

        // The input layout mirrors aver::ui::UiVertex field for field; the static_asserts below
        // are what keep the literal offsets and the struct in step.
        gd.vertexLayout.stride = sizeof(aver::ui::UiVertex);
        gd.vertexLayout.attribCount = 3;
        gd.vertexLayout.attribs[0] = {rhi::VertexSemantic::Position, 0, rhi::Format::RG32Float,   0};
        gd.vertexLayout.attribs[1] = {rhi::VertexSemantic::TexCoord, 0, rhi::Format::RG32Float,   8};
        gd.vertexLayout.attribs[2] = {rhi::VertexSemantic::Color,    0, rhi::Format::RGBA8Unorm, 16};
        static_assert(sizeof(aver::ui::UiVertex) == 20, "the UI input layout below names these offsets");
        static_assert(offsetof(aver::ui::UiVertex, u) == 8, "TEXCOORD0 is declared at byte 8");
        static_assert(offsetof(aver::ui::UiVertex, rgba) == 16, "COLOR0 is declared at byte 16");

        gd.layout.srvCount = 1;                        // t0: the quad's texture
        gd.layout.samplerCount = 1;
        gd.layout.samplers[0].filter  = rhi::Filter::Linear;
        gd.layout.samplers[0].address = rhi::AddressMode::Clamp;
        gd.layout.constantDwords[kUiConstantSlot] = 4; // b3: gUiProj, as root constants

        gd.depth.test = false;
        gd.depth.write = false;
        gd.cull = rhi::CullMode::None;
        gd.blend = rhi::BlendMode::PremultipliedAlpha;
        gd.renderTargetCount = 1;
        gd.renderTargets[0] = device_ ? device_->displayFormat() : rhi::Format::RGBA8Unorm; // the backbuffer this pass draws onto
        gd.depthFormat = rhi::Format::Unknown;
        gd.sampleCount = 1;

        pipeline_ = res_->createGraphicsPipeline(gd);
        if (!pipeline_) { AVER_ERROR("[Render.UI] the UI pipeline failed to create"); return false; }
    }

    if (!ensureCapacity(kInitialVertices, kInitialIndices)) return false;

    AVER_INFO("[Render.UI] ready");
    return true;
}

// Grows every frame's vertex and index buffer to hold at least this much. False on allocation failure.
bool UiRenderer::ensureCapacity(u32 vertexCount, u32 indexCount) {
    const bool growV = vertexCount > vbCapacity_;
    const bool growI = indexCount > ibCapacity_;
    if (!growV && !growI) return true;

    const u32 newV = growV ? vertexCount : vbCapacity_;
    const u32 newI = growI ? indexCount : ibCapacity_;

    for (u32 i = 0; i < kFramesInFlight; ++i) {
        if (growV) {
            res_->destroyBuffer(vb_[i]);
            rhi::BufferDesc bd;
            bd.bytes = static_cast<u64>(newV) * sizeof(aver::ui::UiVertex);
            bd.kind  = rhi::BufferKind::Upload;
            bd.debugName = "Aver.UI vertices";
            vb_[i] = res_->createBuffer(bd);
            if (!vb_[i]) { AVER_ERROR("[Render.UI] UI vertex buffer allocation failed"); return false; }
        }
        if (growI) {
            res_->destroyBuffer(ib_[i]);
            rhi::BufferDesc bd;
            bd.bytes = static_cast<u64>(newI) * sizeof(u32);
            bd.kind  = rhi::BufferKind::Upload;
            bd.debugName = "Aver.UI indices";
            ib_[i] = res_->createBuffer(bd);
            if (!ib_[i]) { AVER_ERROR("[Render.UI] UI index buffer allocation failed"); return false; }
        }
    }
    vbCapacity_ = newV;
    ibCapacity_ = newI;
    return true;
}

// The binding set for a texture, created on first use and cached. 0 when creation failed.
rhi::BindingSetHandle UiRenderer::bindingFor(rhi::TextureHandle t) {
    for (const TexBinding& b : bindings_) if (b.texture == t) return b.set;

    rhi::BindingSetDesc bd;
    bd.srvCount = 1;
    bd.srvKinds[0] = rhi::SlotKind::Texture2D;
    bd.srvBaseRegister = 0;
    const rhi::BindingSetHandle set = res_->createBindingSet(bd);
    if (!set) return 0;
    res_->setSrv(set, 0, t, rhi::kAllMips);
    bindings_.push_back({t, set});
    return set;
}

// Forgets a texture's binding set (the texture is about to be destroyed).
void UiRenderer::dropBinding(rhi::TextureHandle t) {
    for (usize i = 0; i < bindings_.size(); ++i) {
        if (bindings_[i].texture != t) continue;
        res_->destroyBindingSet(bindings_[i].set);
        bindings_.erase(bindings_.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

void UiRenderer::setTextAtlas(aver::ui::UiGlyphAtlas* atlas) { atlas_ = atlas; }

rhi::TextureHandle UiRenderer::pageTexture(u32 page) const { return page < pages_.size() ? pages_[page].tex : 0; }

void UiRenderer::prePass(rhi::IRenderContext& ctx) {
    if (!atlas_ || !res_) return;
    const u32 count = atlas_->pageCount();
    const u32 size = atlas_->pageSize();
    if (pages_.size() < count) pages_.resize(count);

    struct Job {
        u32 page;
        rhi::TextureHandle tex;
        rhi::TextureCopyFootprint fp;
        u64 offset;
    };
    std::vector<Job> jobs;
    u64 total = 0;
    for (u32 i = 0; i < count; ++i) {
        if (!atlas_->pageDirty(i)) continue;
        PageTexture& pt = pages_[i];
        if (pt.tex && pt.size != size) {   // the page size changed: the old texture is the wrong shape
            dropBinding(pt.tex);
            res_->destroyTexture(pt.tex);
            pt = PageTexture{};
        }
        if (!pt.tex) {
            const std::string name = "Aver.UI glyph page " + std::to_string(i);
            rhi::TextureDesc td;
            td.width = td.height = size;
            td.format = rhi::Format::RGBA8Unorm;
            td.bind = rhi::ResourceBind::ShaderResource;
            td.initialState = rhi::ResourceState::ShaderResource;
            td.debugName = name.c_str();
            pt.tex = res_->createTexture(td);
            if (!pt.tex) { AVER_ERROR("[Render.UI] could not create glyph atlas page {}", i); continue; }
            pt.size = size;
        }
        rhi::TextureCopyFootprint fp;
        if (!res_->textureCopyFootprint(pt.tex, 0, fp) || fp.rowBytes != size * 4 || fp.rowPitch < fp.rowBytes ||
            fp.rows != size)
            continue;
        total = (total + 511) & ~static_cast<u64>(511);   // D3D12 wants 512-byte placement offsets
        jobs.push_back(Job{i, pt.tex, fp, total});
        total += fp.totalBytes;
    }
    if (jobs.empty()) {
        atlas_->setUploadBytes(0);
        return;
    }

    rhi::BufferDesc bd;
    bd.bytes = total;
    bd.kind = rhi::BufferKind::Upload;
    bd.debugName = "Aver.UI glyph upload";
    const rhi::BufferHandle staging = res_->createBuffer(bd);
    if (!staging) { AVER_ERROR("[Render.UI] glyph upload buffer allocation failed"); return; }

    // coverage -> premultiplied white: (c, c, c, c), optionally through a gamma curve
    u8 lut[256];
    const f32 gamma = atlas_->coverageGamma() > 0.0f ? atlas_->coverageGamma() : 1.0f;
    for (int v = 0; v < 256; ++v)
        lut[v] = gamma == 1.0f ? static_cast<u8>(v)
                               : static_cast<u8>(std::clamp(std::pow(static_cast<f32>(v) / 255.0f, gamma) * 255.0f + 0.5f, 0.0f, 255.0f));

    u32 uploaded = 0;
    std::vector<u8> block;
    for (const Job& j : jobs) {
        const u8* src = atlas_->pagePixels(j.page);
        if (!src) continue;
        // Rows at rowPitch, NOT tightly packed: the padding is real on D3D12 (256-byte row alignment).
        block.assign(static_cast<usize>(j.fp.totalBytes), 0);
        for (u32 y = 0; y < size; ++y) {
            u8* dst = block.data() + static_cast<usize>(y) * j.fp.rowPitch;
            const u8* row = src + static_cast<usize>(y) * size;
            for (u32 x = 0; x < size; ++x) {
                const u8 c = lut[row[x]];
                dst[x * 4 + 0] = c; dst[x * 4 + 1] = c; dst[x * 4 + 2] = c; dst[x * 4 + 3] = c;
            }
        }
        if (!res_->writeBuffer(staging, block.data(), j.fp.totalBytes, j.offset)) continue;
        ctx.textureBarrier(j.tex, rhi::ResourceState::ShaderResource, rhi::ResourceState::CopyDest);
        ctx.copyBufferToTexture(j.tex, 0, staging, j.offset);
        ctx.textureBarrier(j.tex, rhi::ResourceState::CopyDest, rhi::ResourceState::ShaderResource);
        atlas_->markUploaded(j.page);
        uploaded += size * size * 4;
    }
    // The copies are recorded, not run: destroying the staging buffer here is fence-safe (the backends
    // retire it behind the frame), the same as VoxiRenderer's GI-cache restore.
    res_->destroyBuffer(staging);
    atlas_->setUploadBytes(uploaded);
}

// Copies a draw list in, flattening its layers low band to high into draw order.
void UiRenderer::submit(const aver::ui::UiDrawList& list) {
    verts_.clear();
    idx_.clear();
    draws_.clear();
    if (list.empty()) return;

    verts_ = list.vertices();
    idx_   = list.indices();

    for (u8 l = 0; l < static_cast<u8>(aver::ui::UiLayer::Count); ++l) {
        for (const aver::ui::UiDrawCmd& c : list.commands(static_cast<aver::ui::UiLayer>(l))) {
            if (c.indexCount == 0 || c.clip.empty()) continue;
            draws_.push_back({c.indexOffset, c.indexCount, c.texture, c.clip});
        }
    }
}

// Draws the submitted list onto the backbuffer, in submission order, with no depth.
void UiRenderer::overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) {
    if (draws_.empty() || !pipeline_ || width == 0 || height == 0) return;
    if (!ensureCapacity(static_cast<u32>(verts_.size()), static_cast<u32>(idx_.size()))) return;

    // Rotated here, not in submit(), so two submits in one frame cannot advance the ring twice.
    frame_ = (frame_ + 1) % kFramesInFlight;
    const rhi::BufferHandle vb = vb_[frame_];
    const rhi::BufferHandle ib = ib_[frame_];
    if (!vb || !ib) return;

    if (!res_->writeBuffer(vb, verts_.data(), verts_.size() * sizeof(aver::ui::UiVertex))) return;
    if (!res_->writeBuffer(ib, idx_.data(), idx_.size() * sizeof(u32))) return;

    ctx.pushMarker("Aver.UI");
    ctx.setPipeline(pipeline_);

    // Pixels to NDC with the origin TOP-LEFT: the y flip lives in these four floats and nowhere else.
    const f32 proj[4] = {2.0f / static_cast<f32>(width), -2.0f / static_cast<f32>(height), -1.0f, 1.0f};
    ctx.setConstants(kUiConstantSlot, proj, 4);
    ctx.setViewport(0, 0, width, height);

    ctx.setVertexBuffer(vb, sizeof(aver::ui::UiVertex));
    ctx.setIndexBuffer(ib, rhi::Format::R32Uint);

    rhi::TextureHandle boundTexture = 0;
    bool haveBound = false;
    for (const Draw& d : draws_) {
        rhi::TextureHandle tex = white_;
        if (d.texture & aver::ui::kUiAtlasTokenBit) {
            // A glyph atlas page; its texture exists once prePass has uploaded the page.
            const u32 page = static_cast<u32>(d.texture & 0xFFFFu);
            tex = page < pages_.size() ? pages_[page].tex : 0;
            if (!tex) continue;
        } else if (d.texture) {
            tex = static_cast<rhi::TextureHandle>(d.texture);
        }
        if (!haveBound || tex != boundTexture) {
            const rhi::BindingSetHandle set = bindingFor(tex);
            if (!set) continue;
            ctx.setBindingSet(set, 0);
            boundTexture = tex;
            haveBound = true;
        }

        // The clip rect is the scissor, clamped to the backbuffer.
        const i32 l = d.clip.left   < 0 ? 0 : d.clip.left;
        const i32 t = d.clip.top    < 0 ? 0 : d.clip.top;
        const i32 r = d.clip.right  > static_cast<i32>(width)  ? static_cast<i32>(width)  : d.clip.right;
        const i32 b = d.clip.bottom > static_cast<i32>(height) ? static_cast<i32>(height) : d.clip.bottom;
        if (r <= l || b <= t) continue;
        ctx.setScissor(static_cast<u32>(l), static_cast<u32>(t),
                       static_cast<u32>(r - l), static_cast<u32>(b - t));

        ctx.drawIndexed(d.indexCount, d.indexOffset, 0);
    }
    ctx.popMarker();
}

} // namespace aver::render::ui
