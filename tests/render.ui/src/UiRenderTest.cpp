// The UI renderer, verified against a recording device rather than a GPU: layer order, binding
// reuse, the scissor, which buffer of the ring a frame writes, growth, and the bytes uploaded.
// Exit code = failure count.
#include "aver/render/ui/UiRenderer.hpp"
#include "aver/core/Log.hpp"

#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

// Records one assertion. Counts a failure and logs it when the condition is false.
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

namespace {

// ---------------------------------------------------------------- the recording device

// A recorded buffer creation.
struct BufferRecord {
    u64 bytes = 0;
    rhi::BufferKind kind = rhi::BufferKind::Default;
    std::vector<u8> contents;   // whatever writeBuffer last put in it
};

// Resource factory that records every creation instead of touching a GPU. Handles count from 1.
struct MockFactory final : public rhi::IResourceFactory {
    std::vector<rhi::TextureDesc>          textures;
    std::vector<BufferRecord>              buffers;
    std::vector<rhi::ShaderDesc>           shaders;
    std::vector<rhi::GraphicsPipelineDesc> pipelines;
    std::vector<rhi::BindingSetDesc>       sets;
    std::vector<rhi::TextureHandle>        srvSlot0;    // parallel to `sets`: what slot 0 holds
    u32 destroyedBuffers = 0;

    rhi::TextureHandle createTexture(const rhi::TextureDesc& d) override {
        textures.push_back(d);
        return static_cast<rhi::TextureHandle>(textures.size());
    }
    rhi::BufferHandle createBuffer(const rhi::BufferDesc& d) override {
        buffers.push_back(BufferRecord{d.bytes, d.kind, {}});
        return static_cast<rhi::BufferHandle>(buffers.size());
    }
    rhi::ShaderHandle createShader(const rhi::ShaderDesc& d) override {
        shaders.push_back(d);
        return static_cast<rhi::ShaderHandle>(shaders.size());
    }
    rhi::PipelineHandle createGraphicsPipeline(const rhi::GraphicsPipelineDesc& d) override {
        pipelines.push_back(d);
        return static_cast<rhi::PipelineHandle>(pipelines.size());
    }
    rhi::PipelineHandle createComputePipeline(const rhi::ComputePipelineDesc&) override { return 0; }
    rhi::BindingSetHandle createBindingSet(const rhi::BindingSetDesc& d) override {
        sets.push_back(d);
        srvSlot0.push_back(0);
        return static_cast<rhi::BindingSetHandle>(sets.size());
    }
    rhi::BlasHandle createBlas(rhi::MeshHandle) override { return 0; }
    rhi::TlasHandle createTlas(u32) override { return 0; }

    void destroyTexture(rhi::TextureHandle) override {}
    void destroyBuffer(rhi::BufferHandle h) override { if (h) ++destroyedBuffers; }
    void destroyShader(rhi::ShaderHandle) override {}
    void destroyPipeline(rhi::PipelineHandle) override {}
    void destroyBindingSet(rhi::BindingSetHandle) override {}

    void setSrv(rhi::BindingSetHandle set, u32 slot, rhi::TextureHandle t, u32) override {
        if (set && set <= srvSlot0.size() && slot == 0) srvSlot0[set - 1] = t;
    }
    void setUav(rhi::BindingSetHandle, u32, rhi::TextureHandle, u32) override {}
    void setSrvTlas(rhi::BindingSetHandle, u32, rhi::TlasHandle) override {}
    void setSrvBuffer(rhi::BindingSetHandle, u32, rhi::BufferHandle, u32, u32, u32) override {}
    void setUavBuffer(rhi::BindingSetHandle, u32, rhi::BufferHandle, u32, u32, u32) override {}
    bool readBuffer(rhi::BufferHandle, void*, u64, u64) override { return true; }

    bool writeBuffer(rhi::BufferHandle h, const void* src, u64 bytes, u64 offset) override {
        if (h == 0 || h > buffers.size()) return false;
        BufferRecord& b = buffers[h - 1];
        if (offset + bytes > b.bytes) return false;
        if (b.contents.size() < offset + bytes) b.contents.resize(static_cast<usize>(offset + bytes));
        const u8* s = static_cast<const u8*>(src);
        for (u64 i = 0; i < bytes; ++i) b.contents[static_cast<usize>(offset + i)] = s[static_cast<usize>(i)];
        return true;
    }
    bool textureInfo(rhi::TextureHandle, rhi::TextureDesc&) const override { return false; }
    void waitIdle() override {}
};

// One recorded render-context call, with up to four of its arguments.
struct Call {
    enum class Kind { Pipeline, Viewport, Scissor, BindingSet, Constants, VertexBuffer, IndexBuffer, DrawIndexed };
    Kind kind;
    u32 a = 0, b = 0, c = 0, d = 0;
};

// Render context that appends every call it receives to one flat log, in order.
struct MockContext final : public rhi::IRenderContext {
    std::vector<Call> calls;

    void setPipeline(rhi::PipelineHandle p) override { calls.push_back({Call::Kind::Pipeline, p}); }
    void setViewport(u32 x, u32 y, u32 w, u32 h) override { calls.push_back({Call::Kind::Viewport, x, y, w, h}); }
    void setScissor(u32 x, u32 y, u32 w, u32 h) override { calls.push_back({Call::Kind::Scissor, x, y, w, h}); }
    void setRenderTargets(const rhi::TextureHandle*, u32, rhi::TextureHandle) override {}
    void clearDepth(rhi::TextureHandle, f32) override {}
    void clearColor(rhi::TextureHandle, const f32[4]) override {}
    void setBindingSet(rhi::BindingSetHandle s, u32 table) override { calls.push_back({Call::Kind::BindingSet, s, table}); }
    void setConstants(u32 slot, const void*, u32 dwords) override { calls.push_back({Call::Kind::Constants, slot, dwords}); }
    void setConstantBuffer(u32, const void*, u32) override {}
    void drawMesh(rhi::MeshHandle) override {}
    void dispatchMeshFor(rhi::MeshHandle) override {}
    void dispatch(u32, u32, u32) override {}
    void copyBuffer(rhi::BufferHandle, rhi::BufferHandle, u64, u64, u64) override {}
    void copyTexture(rhi::TextureHandle, rhi::TextureHandle) override {}
    void drawFullscreen() override {}
    void setVertexBuffer(rhi::BufferHandle b, u32 stride) override { calls.push_back({Call::Kind::VertexBuffer, b, stride}); }
    void setIndexBuffer(rhi::BufferHandle b, rhi::Format f) override {
        calls.push_back({Call::Kind::IndexBuffer, b, static_cast<u32>(f)});
    }
    void drawIndexed(u32 indexCount, u32 firstIndex, i32 baseVertex) override {
        calls.push_back({Call::Kind::DrawIndexed, indexCount, firstIndex, static_cast<u32>(baseVertex)});
    }
    void buildBlas(rhi::BlasHandle) override {}
    void buildTlas(rhi::TlasHandle, const rhi::TlasInstance*, u32) override {}
    void textureBarrier(rhi::TextureHandle, rhi::ResourceState, rhi::ResourceState, u32) override {}
    void bufferBarrier(rhi::BufferHandle, rhi::ResourceState, rhi::ResourceState) override {}
    void uavBarrierTexture(rhi::TextureHandle) override {}
    void uavBarrierBuffer(rhi::BufferHandle) override {}

    // Every recorded call of one kind, in the order it was made.
    std::vector<Call> ofKind(Call::Kind k) const {
        std::vector<Call> out;
        for (const Call& c : calls) if (c.kind == k) out.push_back(c);
        return out;
    }
    usize count(Call::Kind k) const { return ofKind(k).size(); }
};

// Device that hands out the recording factory and does nothing else.
struct MockDevice final : public rhi::IDevice {
    MockFactory factory;
    rhi::Backend backend() const override { return rhi::Backend::Null; }
    const char* adapterName() const override { return "recording device"; }
    rhi::IResourceFactory* resources() override { return &factory; }
    rhi::ISwapchain* createSwapchain(const rhi::SwapchainDesc&) override { return nullptr; }
    void beginFrame() override {}
    void endFrame() override {}
};

} // namespace

// Runs every UI renderer check. Returns 1 if any failed.
int main() {
    MockDevice dev;
    render::ui::UiRenderer* r = render::ui::UiRenderer::create(dev);
    if (!r) { AVER_ERROR("=== UiRenderer::create declined against the recording device ==="); return 1; }

    AVER_INFO("=== what init creates ===");
    {
        const MockFactory& f = dev.factory;
        check(f.textures.size() == 1, "one texture: the 1x1 white texel");
        check(f.textures[0].width == 1 && f.textures[0].height == 1, "and it is 1x1");
        check(f.textures[0].format == rhi::Format::RGBA8Unorm, "the white texel is UNORM, not sRGB");
        check(f.textures[0].initialDataCount == 1, "it is uploaded at creation, not left undefined");

        check(f.shaders.size() == 2, "two shaders");
        check(f.shaders[0].stage == rhi::ShaderStage::Vertex && f.shaders[1].stage == rhi::ShaderStage::Pixel,
              "a vertex shader and a pixel shader");
        check(f.shaders[0].prelude == nullptr, "the UI shaders carry no scene prelude");

        check(f.pipelines.size() == 1, "one pipeline: solid and textured share it");
        const rhi::GraphicsPipelineDesc& p = f.pipelines[0];
        check(p.blend == rhi::BlendMode::PremultipliedAlpha, "it blends PREMULTIPLIED source colour");
        check(!p.depth.test && !p.depth.write, "no depth test and no depth write");
        check(p.depthFormat == rhi::Format::Unknown, "and no depth format, because the pass binds none");
        check(p.cull == rhi::CullMode::None, "nothing is culled: a UI quad has no meaningful winding");
        check(p.sampleCount == 1, "the backbuffer is never multisampled");
        check(p.renderTargetCount == 1 && p.renderTargets[0] == rhi::Format::RGBA8Unorm,
              "one target, matching the backbuffer");

        // The vertex layout is an ABI between the UiVertex struct and the input assembler.
        check(p.vertexLayout.attribCount == 3, "three vertex attributes");
        check(p.vertexLayout.stride == 20, "a 20-byte vertex");
        check(p.vertexLayout.attribs[0].format == rhi::Format::RG32Float && p.vertexLayout.attribs[0].offset == 0,
              "POSITION0 is two floats at byte 0");
        check(p.vertexLayout.attribs[1].format == rhi::Format::RG32Float && p.vertexLayout.attribs[1].offset == 8,
              "TEXCOORD0 is two floats at byte 8");
        check(p.vertexLayout.attribs[2].format == rhi::Format::RGBA8Unorm && p.vertexLayout.attribs[2].offset == 16,
              "COLOR0 is four bytes at byte 16");
        check(p.layout.srvCount == 1 && p.layout.samplerCount == 1, "one texture slot and one sampler");

        check(f.buffers.size() == 6, "six buffers: three vertex and three index");
        for (const BufferRecord& b : f.buffers)
            if (b.kind != rhi::BufferKind::Upload) { check(false, "every UI buffer is CPU-writable"); break; }
        check(f.buffers[0].kind == rhi::BufferKind::Upload, "every UI buffer is CPU-writable");
    }

    AVER_INFO("=== an empty list draws nothing ===");
    {
        MockContext ctx;
        aver::ui::UiDrawList empty;
        r->submit(empty);
        r->overlayPass(ctx, 1280, 720);
        check(ctx.calls.empty(), "no pipeline, no draw, not even a viewport");
    }

    AVER_INFO("=== layer order is draw order ===");
    {
        // Authored out of band order on purpose.
        aver::ui::UiDrawList dl;
        dl.setLayer(aver::ui::UiLayer::Tooltip);   dl.addRect(0, 0, 10, 10, 0xFFFFFFFF);
        dl.setLayer(aver::ui::UiLayer::Background);dl.addRect(0, 0, 10, 10, 0xFFFFFFFF);
        dl.setLayer(aver::ui::UiLayer::Content);   dl.addRect(0, 0, 10, 10, 0xFFFFFFFF);

        MockContext ctx;
        r->submit(dl);
        r->overlayPass(ctx, 1280, 720);

        const std::vector<Call> draws = ctx.ofKind(Call::Kind::DrawIndexed);
        check(draws.size() == 3, "three draws");
        check(dev.factory.sets.size() == 1, "the white texel's binding set is created on first use");
        check(ctx.count(Call::Kind::BindingSet) == 1, "and bound once for all three draws");
        // Each rect contributed 6 indices in authoring order, so its index offset names it.
        check(draws[0].b == 6,  "background (authored 2nd, offset 6) is drawn FIRST");
        check(draws[1].b == 12, "content (authored 3rd, offset 12) is drawn second");
        check(draws[2].b == 0,  "tooltip (authored 1st, offset 0) is drawn LAST");
        for (const Call& d : draws) check(d.a == 6, "each draw covers its own six indices");
    }

    AVER_INFO("=== bindings, and the white texel ===");
    {
        const usize setsBefore = dev.factory.sets.size();
        aver::ui::UiDrawList dl;
        dl.addRect(0, 0, 10, 10, 0xFFFFFFFF);                                   // untextured
        dl.addTexturedRect(0, 0, 10, 10, /*texture*/ 1, 0, 0, 1, 1, 0xFFFFFFFF); // the white texel's own handle
        dl.addRect(20, 0, 10, 10, 0xFFFFFFFF);                                   // untextured again

        MockContext ctx;
        r->submit(dl);
        r->overlayPass(ctx, 1280, 720);

        // Texture 0 resolves to the white texel, whose handle is 1 here, so both land on one binding.
        check(dev.factory.sets.size() == setsBefore, "no new binding set: the cache outlives the frame");
        check(ctx.count(Call::Kind::BindingSet) == 1, "and it is bound once, not once per draw");
        check(dev.factory.srvSlot0.back() == 1, "slot 0 holds the white texture");
        check(ctx.count(Call::Kind::DrawIndexed) == 3, "still three draws: batching is the list's job, not the renderer's");
    }

    AVER_INFO("=== a second texture gets its own binding ===");
    {
        const usize setsBefore = dev.factory.sets.size();
        aver::ui::UiDrawList dl;
        dl.addTexturedRect(0, 0, 10, 10, 7, 0, 0, 1, 1, 0xFFFFFFFF);
        dl.addTexturedRect(0, 0, 10, 10, 9, 0, 0, 1, 1, 0xFFFFFFFF);

        MockContext ctx;
        r->submit(dl);
        r->overlayPass(ctx, 1280, 720);
        check(dev.factory.sets.size() == setsBefore + 2, "two new textures, two new binding sets");
        check(ctx.count(Call::Kind::BindingSet) == 2, "and each is bound before its own draw");
    }

    AVER_INFO("=== the clip becomes the scissor ===");
    {
        aver::ui::UiDrawList dl;
        dl.pushClip(aver::ui::UiClip{100, 50, 300, 200});
        dl.addRect(0, 0, 10, 10, 0xFFFFFFFF);
        dl.popClip();
        // Deliberately larger than the screen.
        dl.pushClip(aver::ui::UiClip{-500, -500, 5000, 5000});
        dl.addRect(0, 0, 10, 10, 0xFFFFFFFF);
        dl.popClip();

        MockContext ctx;
        r->submit(dl);
        r->overlayPass(ctx, 1280, 720);

        const std::vector<Call> sc = ctx.ofKind(Call::Kind::Scissor);
        check(sc.size() == 2, "one scissor per draw");
        check(sc[0].a == 100 && sc[0].b == 50 && sc[0].c == 200 && sc[0].d == 150,
              "the clip rect arrives as x,y,width,height");
        check(sc[1].a == 0 && sc[1].b == 0 && sc[1].c == 1280 && sc[1].d == 720,
              "a clip larger than the screen is CLAMPED to it, not passed through");
    }

    AVER_INFO("=== the buffer ring rotates ===");
    {
        aver::ui::UiDrawList dl;
        dl.addRect(0, 0, 10, 10, 0xFFFFFFFF);
        r->submit(dl);

        u32 seen[4] = {};
        for (int i = 0; i < 4; ++i) {
            MockContext ctx;
            r->overlayPass(ctx, 1280, 720);
            const std::vector<Call> vb = ctx.ofKind(Call::Kind::VertexBuffer);
            seen[i] = vb.empty() ? 0u : vb[0].a;
            check(!vb.empty() && vb[0].b == 20, "the stride bound is the 20-byte UiVertex");
        }
        check(seen[0] && seen[1] && seen[2], "three frames, three buffers");
        check(seen[0] != seen[1] && seen[1] != seen[2] && seen[0] != seen[2],
              "consecutive frames write DIFFERENT buffers -- the CPU never overwrites one in flight");
        check(seen[3] == seen[0], "and the fourth frame comes back round to the first");
    }

    AVER_INFO("=== the bytes that reach the GPU are the bytes in the list ===");
    {
        aver::ui::UiDrawList dl;
        dl.addRect(11, 22, 33, 44, 0xFF203040);

        MockContext ctx;
        r->submit(dl);
        r->overlayPass(ctx, 1280, 720);

        const std::vector<Call> vb = ctx.ofKind(Call::Kind::VertexBuffer);
        check(!vb.empty(), "a vertex buffer was bound");
        if (!vb.empty()) {
            const BufferRecord& rec = dev.factory.buffers[vb[0].a - 1];
            check(rec.contents.size() >= 4 * sizeof(aver::ui::UiVertex), "four vertices were uploaded");
            aver::ui::UiVertex v0{};
            std::memcpy(&v0, rec.contents.data(), sizeof(v0));
            check(v0.x == 11.0f && v0.y == 22.0f, "the first vertex is the rect's top-left corner");
            check(v0.rgba == aver::ui::uiPremultiply(0xFF203040), "the colour is uploaded premultiplied");
        }
        const std::vector<Call> ib = ctx.ofKind(Call::Kind::IndexBuffer);
        check(!ib.empty() && ib[0].b == static_cast<u32>(rhi::Format::R32Uint), "indices are 32-bit");
    }

    AVER_INFO("=== the projection, and the viewport ===");
    {
        aver::ui::UiDrawList dl;
        dl.addRect(0, 0, 10, 10, 0xFFFFFFFF);
        MockContext ctx;
        r->submit(dl);
        r->overlayPass(ctx, 800, 600);

        const std::vector<Call> vp = ctx.ofKind(Call::Kind::Viewport);
        check(vp.size() == 1 && vp[0].c == 800 && vp[0].d == 600, "the viewport is the whole backbuffer");
        const std::vector<Call> k = ctx.ofKind(Call::Kind::Constants);
        // b0/b1/b2/b4 are reserved by the RHI; b3 is the only slot a feature may claim.
        check(k.size() == 1 && k[0].a == 3 && k[0].b == 4, "four constant dwords at slot b3");
    }

    AVER_INFO("=== growth keeps the draws ===");
    {
        // The initial capacity is 4096 vertices; 1200 rects is 4800.
        aver::ui::UiDrawList dl;
        for (int i = 0; i < 1200; ++i) dl.addRect(f32(i), 0, 2, 2, 0xFFFFFFFF);
        check(dl.vertices().size() == 4800, "the list really is over the initial capacity");

        const u32 destroyedBefore = dev.factory.destroyedBuffers;
        MockContext ctx;
        r->submit(dl);
        r->overlayPass(ctx, 1280, 720);

        check(dev.factory.destroyedBuffers > destroyedBefore, "the old buffers were released, not leaked");
        const std::vector<Call> draws = ctx.ofKind(Call::Kind::DrawIndexed);
        check(draws.size() == 1 && draws[0].a == 7200, "one merged draw covering all 7200 indices");
        const std::vector<Call> vb = ctx.ofKind(Call::Kind::VertexBuffer);
        check(!vb.empty() && dev.factory.buffers[vb[0].a - 1].bytes >= 4800 * 20,
              "and the buffer it drew from is big enough to hold them");
    }

    AVER_INFO("=== a stale list does not linger ===");
    {
        // Submitting an empty list must clear the UI.
        aver::ui::UiDrawList empty;
        MockContext ctx;
        r->submit(empty);
        r->overlayPass(ctx, 1280, 720);
        check(ctx.count(Call::Kind::DrawIndexed) == 0, "the previous frame's draws are gone");
    }

    delete r;

    if (g_failures == 0) AVER_INFO("=== all UI renderer tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
