#include "aver/render/ui/UiRenderer.hpp"
#include "UiShaders.hpp"

#include "aver/core/Log.hpp"

#include <cstring>

namespace aver::render::ui {
namespace {

// Starting capacity, and the only number here that is a guess rather than a consequence. A HUD with
// a dozen panels and a few hundred glyphs is under this; anything larger grows the buffer once and
// stays grown, so the guess costs a single reallocation rather than a per-frame cost.
constexpr u32 kInitialVertices = 4096;
constexpr u32 kInitialIndices  = 6144;

// The register the shader declares its constants at; see the note in UiShaders.hpp for why b3.
constexpr u32 kUiConstantSlot = 3;

} // namespace

UiRenderer* UiRenderer::create(rhi::IDevice& device) {
    UiRenderer* r = new UiRenderer();
    if (!r->init(device)) { delete r; return nullptr; }
    return r;
}

UiRenderer::~UiRenderer() {
    if (!res_) return;
    // Every handle, including the ones a partial init left at zero -- destroy of 0 is a no-op by the
    // handle contract, so there is no need to remember how far init got.
    for (const TexBinding& b : bindings_) res_->destroyBindingSet(b.set);
    for (u32 i = 0; i < kFramesInFlight; ++i) { res_->destroyBuffer(vb_[i]); res_->destroyBuffer(ib_[i]); }
    res_->destroyPipeline(pipeline_);
    res_->destroyShader(vs_);
    res_->destroyShader(ps_);
    res_->destroyTexture(white_);
}

bool UiRenderer::init(rhi::IDevice& device) {
    device_ = &device;
    // A backend with no GPU support returns nullptr. Declining is the contract the other feature
    // modules follow: the engine runs without a UI rather than failing to start.
    res_ = device.resources();
    if (!res_) {
        AVER_WARN("[Render.UI] init declined: backend exposes no resource factory");
        return false;
    }

    // ---- the white texel ----
    // An untextured rect is a textured rect sampling this, which is what keeps the pixel shader
    // branchless and the batching rule uniform: one pipeline, one code path, and a solid quad and a
    // glyph differ only in which texture is bound.
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
        sd.source = kUiHLSL;
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

        // The 20-byte UiVertex, declared here because this is the module that owns both halves of
        // that ABI -- the struct in Aver.UI and the layout the input assembler reads. The offsets
        // are literals for the same reason MeshVertex's are, and are checked below.
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

        // No depth at all -- not a disabled test, no buffer. The overlay pass binds none, and a
        // pipeline declaring a depth format it will never be given fails to create.
        gd.depth.test = false;
        gd.depth.write = false;
        gd.cull = rhi::CullMode::None;                 // a UI quad has no meaningful winding
        // PREMULTIPLIED, matching what uiPremultiply already did to every vertex colour on the way
        // into the list. AlphaBlend would multiply by alpha a second time, which leaves every
        // translucent panel darker than authored while still looking like a plausible panel.
        gd.blend = rhi::BlendMode::PremultipliedAlpha;
        gd.renderTargetCount = 1;
        gd.renderTargets[0] = rhi::Format::RGBA8Unorm; // the backbuffer, which this pass draws onto
        gd.depthFormat = rhi::Format::Unknown;
        gd.sampleCount = 1;                            // the backbuffer is never multisampled

        pipeline_ = res_->createGraphicsPipeline(gd);
        if (!pipeline_) { AVER_ERROR("[Render.UI] the UI pipeline failed to create"); return false; }
    }

    if (!ensureCapacity(kInitialVertices, kInitialIndices)) return false;

    AVER_INFO("[Render.UI] ready");
    return true;
}

bool UiRenderer::ensureCapacity(u32 vertexCount, u32 indexCount) {
    // Grown in one step to what is asked for rather than doubled: a UI's vertex count is bounded by
    // what is on screen, so it settles after a frame or two and a growth factor would only ever
    // overshoot the steady state it converges to.
    const bool growV = vertexCount > vbCapacity_;
    const bool growI = indexCount > ibCapacity_;
    if (!growV && !growI) return true;

    const u32 newV = growV ? vertexCount : vbCapacity_;
    const u32 newI = growI ? indexCount : ibCapacity_;

    for (u32 i = 0; i < kFramesInFlight; ++i) {
        if (growV) {
            // Destroyed BEFORE the replacement is created, which is safe only because the RHI
            // retires a destroyed resource once the GPU has passed every frame that could still
            // reference it. Doing it the other way round would double the peak footprint for no gain.
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

void UiRenderer::submit(const aver::ui::UiDrawList& list) {
    verts_.clear();
    idx_.clear();
    draws_.clear();
    if (list.empty()) return;

    verts_ = list.vertices();
    idx_   = list.indices();

    // LAYER ORDER IS THE DRAW ORDER, and flattening it here is what makes that true. The layers
    // share one vertex buffer and partition only the commands (see UiDrawList), so this walks them
    // low band to high and appends -- background first, debug last -- and the GPU then draws in
    // submission order with no depth and no sorting anywhere. That is the entire compositing model.
    for (u8 l = 0; l < static_cast<u8>(aver::ui::UiLayer::Count); ++l) {
        for (const aver::ui::UiDrawCmd& c : list.commands(static_cast<aver::ui::UiLayer>(l))) {
            if (c.indexCount == 0 || c.clip.empty()) continue;
            draws_.push_back({c.indexOffset, c.indexCount,
                              static_cast<rhi::TextureHandle>(c.texture), c.clip});
        }
    }
}

void UiRenderer::overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) {
    if (draws_.empty() || !pipeline_ || width == 0 || height == 0) return;
    if (!ensureCapacity(static_cast<u32>(verts_.size()), static_cast<u32>(idx_.size()))) return;

    // Rotated HERE rather than in submit(), because this is the function the backend calls once per
    // frame. Rotating on submit would let an app that submits twice in a frame -- a HUD and a debug
    // overlay built separately -- advance the ring twice and write over a buffer in flight.
    frame_ = (frame_ + 1) % kFramesInFlight;
    const rhi::BufferHandle vb = vb_[frame_];
    const rhi::BufferHandle ib = ib_[frame_];
    if (!vb || !ib) return;

    if (!res_->writeBuffer(vb, verts_.data(), verts_.size() * sizeof(aver::ui::UiVertex))) return;
    if (!res_->writeBuffer(ib, idx_.data(), idx_.size() * sizeof(u32))) return;

    ctx.pushMarker("Aver.UI");
    ctx.setPipeline(pipeline_);

    // Pixels to NDC, with the origin at the TOP-LEFT. The y scale is negative and the y translate is
    // +1, which together put pixel row 0 at the top of the screen -- the flip lives in these four
    // floats and nowhere else, so no widget and no layout ever has to know about it.
    const f32 proj[4] = {2.0f / static_cast<f32>(width), -2.0f / static_cast<f32>(height), -1.0f, 1.0f};
    ctx.setConstants(kUiConstantSlot, proj, 4);
    ctx.setViewport(0, 0, width, height);

    // Bound ONCE, outside the loop. Every draw reads the same two buffers -- that is what the
    // shared vertex buffer in UiDrawList buys -- and the only per-draw state below is the texture,
    // the scissor and an index range.
    ctx.setVertexBuffer(vb, sizeof(aver::ui::UiVertex));
    ctx.setIndexBuffer(ib, rhi::Format::R32Uint);

    rhi::TextureHandle boundTexture = 0;
    bool haveBound = false;
    for (const Draw& d : draws_) {
        // 0 is the sentinel for "no texture", which the white texel makes a real binding. Resolved
        // per draw rather than at submit so a widget can name a texture that did not exist yet when
        // its list was built.
        const rhi::TextureHandle tex = d.texture ? d.texture : white_;
        if (!haveBound || tex != boundTexture) {
            const rhi::BindingSetHandle set = bindingFor(tex);
            if (!set) continue;
            ctx.setBindingSet(set, 0);
            boundTexture = tex;
            haveBound = true;
        }

        // The clip rect is the SCISSOR. Clipping by discarding in the pixel shader would work and
        // would also make every clipped pixel cost a fetch and a branch; the scissor costs nothing
        // and is what the rectangle already is. Clamped to the backbuffer because a widget may
        // legitimately push a clip that extends past the screen and a negative rect is rejected.
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
