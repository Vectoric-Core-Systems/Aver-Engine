// The UI renderer, verified against a recording device rather than a GPU: layer order, binding
// reuse, the scissor, which buffer of the ring a frame writes, growth, and the bytes uploaded.
// Exit code = failure count.
#include "TestCamera.hpp"

#include "aver/render/ui/UiRenderer.hpp"
#include "aver/render/ui/UiImageCache.hpp"
#include "aver/core/Log.hpp"

#include <cstring>
#include <string>
#include <vector>

#include "aver/ui/UiGlyphAtlas.hpp"

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
    std::vector<std::string>               textureNames;       // parallel to `textures` (the desc's name pointer does not outlive the call)
    std::vector<rhi::TextureHandle>        destroyedTextures;
    u32 destroyedBuffers = 0;
    u32 destroyedBindingSets = 0;

    rhi::TextureHandle createTexture(const rhi::TextureDesc& d) override {
        textures.push_back(d);
        textureNames.push_back(d.debugName ? d.debugName : "");
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
    // The ray path's bindless table plays no part in these tests; refuse rather than pretend.
    rhi::BindlessTableHandle createBindlessTextureTable(u32) override { return 0; }
    void destroyBindlessTextureTable(rhi::BindlessTableHandle) override {}
    bool setBindlessTexture(rhi::BindlessTableHandle, u32, rhi::TextureHandle) override { return false; }
    u32  bindlessTableCapacity(rhi::BindlessTableHandle) const override { return 0; }
    rhi::BindingSetHandle createBindingSet(const rhi::BindingSetDesc& d) override {
        sets.push_back(d);
        srvSlot0.push_back(0);
        return static_cast<rhi::BindingSetHandle>(sets.size());
    }
    rhi::BlasHandle createBlas(rhi::MeshHandle) override { return 0; }
    rhi::TlasHandle createTlas(u32) override { return 0; }

    void destroyTexture(rhi::TextureHandle h) override { if (h) destroyedTextures.push_back(h); }
    void destroyBuffer(rhi::BufferHandle h) override { if (h) ++destroyedBuffers; }
    void destroyShader(rhi::ShaderHandle) override {}
    void destroyPipeline(rhi::PipelineHandle) override {}
    void destroyBindingSet(rhi::BindingSetHandle h) override { if (h) ++destroyedBindingSets; }

    void setSrv(rhi::BindingSetHandle set, u32 slot, rhi::TextureHandle t, u32) override {
        if (set && set <= srvSlot0.size() && slot == 0) srvSlot0[set - 1] = t;
    }
    void setUav(rhi::BindingSetHandle, u32, rhi::TextureHandle, u32) override {}
    void clearSrv(rhi::BindingSetHandle, u32) override {}
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
    // Any handle lives until destroyed (the tests also draw with handles they never created).
    bool textureInfo(rhi::TextureHandle t, rhi::TextureDesc& out) const override {
        if (t == 0) return false;
        for (const rhi::TextureHandle d : destroyedTextures) if (d == t) return false;
        out = t <= textures.size() ? textures[t - 1] : rhi::TextureDesc{};
        return true;
    }
    // The copy layout of a 4-byte-per-texel texture with D3D12's 256-byte row alignment.
    bool textureCopyFootprint(rhi::TextureHandle t, u32, rhi::TextureCopyFootprint& out) const override {
        if (t == 0 || t > textures.size()) return false;
        const rhi::TextureDesc& d = textures[t - 1];
        out.rowBytes = d.width * 4;
        out.rowPitch = (out.rowBytes + 255u) & ~255u;
        out.rows = d.height;
        out.depth = 1;
        out.totalBytes = static_cast<u64>(out.rowPitch) * out.rows;
        return true;
    }
    void waitIdle() override {}
};

// One recorded render-context call, with up to four of its arguments.
struct Call {
    enum class Kind { Pipeline, Viewport, Scissor, BindingSet, Constants, VertexBuffer, IndexBuffer, DrawIndexed, Barrier, CopyBufferToTexture };
    Kind kind;
    u32 a = 0, b = 0, c = 0, d = 0;
};

// Render context that appends every call it receives to one flat log, in order.
struct MockContext final : public rhi::IRenderContext {
    std::vector<Call> calls;
    std::vector<f32> lastConstants;   // the dwords of the last setConstants, read as floats

    void setPipeline(rhi::PipelineHandle p) override { calls.push_back({Call::Kind::Pipeline, p}); }
    void setViewport(u32 x, u32 y, u32 w, u32 h) override { calls.push_back({Call::Kind::Viewport, x, y, w, h}); }
    void setScissor(u32 x, u32 y, u32 w, u32 h) override { calls.push_back({Call::Kind::Scissor, x, y, w, h}); }
    void setRenderTargets(const rhi::TextureHandle*, u32, rhi::TextureHandle) override {}
    void clearDepth(rhi::TextureHandle, f32) override {}
    void clearColor(rhi::TextureHandle, const f32[4]) override {}
    void setBindingSet(rhi::BindingSetHandle s, u32 table) override { calls.push_back({Call::Kind::BindingSet, s, table}); }
    void setBindlessTable(rhi::BindlessTableHandle) override {}
    void setConstants(u32 slot, const void* data, u32 dwords) override {
        calls.push_back({Call::Kind::Constants, slot, dwords});
        lastConstants.assign(static_cast<const f32*>(data), static_cast<const f32*>(data) + dwords);
    }
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
    void textureBarrier(rhi::TextureHandle t, rhi::ResourceState from, rhi::ResourceState to, u32) override {
        calls.push_back({Call::Kind::Barrier, t, static_cast<u32>(from), static_cast<u32>(to)});
    }
    void copyBufferToTexture(rhi::TextureHandle dst, u32 mip, rhi::BufferHandle src, u64 offset) override {
        calls.push_back({Call::Kind::CopyBufferToTexture, dst, mip, src, static_cast<u32>(offset)});
    }
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

    // The scene camera and targets the world text pass reads.
    bool haveCamera = false;
    f32 vp[16] = {}, ivp[16] = {}, eyePos[3] = {};
    bool camera(f32 viewProj[16], f32 invViewProjRel[16], f32 cameraPos[3]) const override {
        if (!haveCamera) return false;
        for (int i = 0; i < 16; ++i) { viewProj[i] = vp[i]; invViewProjRel[i] = ivp[i]; }
        for (int i = 0; i < 3; ++i) cameraPos[i] = eyePos[i];
        return true;
    }
    rhi::Format backbufferFormat() const override { return rhi::Format::RGBA16F; }
    rhi::Format depthFormat() const override { return rhi::Format::D32Float; }
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

        check(f.buffers.size() == 10, "ten buffers: five vertex and five index");
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

        // Five deep: two frames in flight, two overlay passes each with frame interpolation on, one spare.
        u32 seen[6] = {};
        for (int i = 0; i < 6; ++i) {
            MockContext ctx;
            r->overlayPass(ctx, 1280, 720);
            const std::vector<Call> vb = ctx.ofKind(Call::Kind::VertexBuffer);
            seen[i] = vb.empty() ? 0u : vb[0].a;
            check(!vb.empty() && vb[0].b == 20, "the stride bound is the 20-byte UiVertex");
        }
        bool allSet = true, allDistinct = true;
        for (int i = 0; i < 5; ++i) {
            allSet = allSet && seen[i] != 0;
            for (int j = i + 1; j < 5; ++j) allDistinct = allDistinct && seen[i] != seen[j];
        }
        check(allSet, "five passes, five buffers");
        check(allDistinct,
              "consecutive passes write DIFFERENT buffers -- the CPU never overwrites one in flight");
        check(seen[5] == seen[0], "and the sixth pass comes back round to the first");
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
        check(k.size() == 1 && k[0].a == 3 && k[0].b == 5, "five constant dwords at slot b3 (projection + colour vision)");
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

    AVER_INFO("=== the glyph atlas: upload, tokens ===");
    {
        MockFactory& f = dev.factory;
        // A 300 pixel page: 1200 bytes a row, so the mock's 256-byte alignment makes the pitch 1280 (80 bytes of padding).
        aver::ui::UiGlyphAtlas atlas({300, 2});
        // Not a real glyph: a 4 x 3 rectangle of coverage 200, wherever the packer puts it.
        aver::ui::UiGlyphBitmap bmp;
        bmp.w = 4;
        bmp.h = 3;
        bmp.pixels.assign(12, 200);
        const aver::ui::UiAtlasGlyph* g = atlas.insert(aver::ui::UiGlyphKey{1, 16, 65}, bmp, 1);
        check(g != nullptr && g->page == 0, "a glyph lands on page 0");

        {   // no atlas lent: nothing happens
            MockContext ctx;
            r->prePass(ctx);
            check(ctx.calls.empty() && f.textures.size() == 1, "prePass with no atlas does nothing");
        }
        r->setTextAtlas(&atlas);
        const usize texturesBefore = f.textures.size();
        const usize buffersBefore = f.buffers.size();
        const u32 destroyedBefore = f.destroyedBuffers;
        {
            MockContext ctx;
            r->prePass(ctx);
            check(f.textures.size() == texturesBefore + 1, "the first dirty page creates exactly one texture");
            const rhi::TextureDesc& td = f.textures.back();
            check(td.width == 300 && td.height == 300 && td.format == rhi::Format::RGBA8Unorm, "RGBA8Unorm, one texel per atlas pixel");
            check(td.initialState == rhi::ResourceState::ShaderResource && any(td.bind, rhi::ResourceBind::ShaderResource), "created in the ShaderResource state");
            check(f.textureNames.back() == "Aver.UI glyph page 0", "and named after its page");
            check(f.buffers.size() == buffersBefore + 1 && f.buffers.back().kind == rhi::BufferKind::Upload, "one upload buffer");
            check(f.buffers.back().bytes == 1280ull * 300ull, "sized to the copy footprint totalBytes");
            const std::vector<Call> barriers = ctx.ofKind(Call::Kind::Barrier);
            const std::vector<Call> copies = ctx.ofKind(Call::Kind::CopyBufferToTexture);
            check(barriers.size() == 2 && copies.size() == 1, "two barriers and one copy");
            check(ctx.calls.size() == 3 && ctx.calls[0].kind == Call::Kind::Barrier && ctx.calls[1].kind == Call::Kind::CopyBufferToTexture &&
                      ctx.calls[2].kind == Call::Kind::Barrier,
                  "in the order barrier, copy, barrier");
            check(barriers[0].b == static_cast<u32>(rhi::ResourceState::ShaderResource) && barriers[0].c == static_cast<u32>(rhi::ResourceState::CopyDest) &&
                      barriers[1].b == static_cast<u32>(rhi::ResourceState::CopyDest) && barriers[1].c == static_cast<u32>(rhi::ResourceState::ShaderResource),
                  "ShaderResource to CopyDest, then back");
            check(copies[0].a == barriers[0].a && copies[0].b == 0 && copies[0].c == f.buffers.size() && copies[0].d == 0,
                  "the copy targets mip 0 of the page from the staging buffer at offset 0");
            check(f.destroyedBuffers == destroyedBefore + 1, "the staging buffer is destroyed as soon as the copy is recorded");

            const std::vector<u8>& bytes = f.buffers.back().contents;
            check(bytes.size() >= 1280ull * 300ull - 1, "the whole footprint was written");
            const aver::ui::UiAtlasGlyph& a = *g;
            const usize px = (static_cast<usize>(a.y) * 1280) + static_cast<usize>(a.x) * 4;
            check(bytes[px] == 200 && bytes[px + 1] == 200 && bytes[px + 2] == 200 && bytes[px + 3] == 200, "coverage 200 became (200, 200, 200, 200): premultiplied white");
            const usize second = (static_cast<usize>(a.y + 1) * 1280) + static_cast<usize>(a.x) * 4;
            check(bytes[second] == 200, "the next row is a full pitch (1280 bytes) later, not 1200");
            check(bytes[px + 4 * 4] == 0 && bytes[1200] == 0 && bytes[1279] == 0, "texels outside the glyph and the row padding are zero");
            check(!atlas.pageDirty(0) && atlas.uploadBytes() == 300u * 300u * 4u, "the page is marked uploaded and the byte count is recorded");
        }
        {   // a clean frame uploads nothing
            MockContext ctx;
            const usize t = f.textures.size(), b = f.buffers.size();
            r->prePass(ctx);
            check(ctx.calls.empty() && f.textures.size() == t && f.buffers.size() == b, "a frame with no dirty page records nothing and allocates nothing");
            check(atlas.uploadBytes() == 0, "and reports zero bytes uploaded");
        }
        {   // a recycled page re-uploads into the same texture
            aver::ui::UiGlyphAtlas one({300, 1});
            aver::ui::UiGlyphBitmap big;
            big.w = 300;
            big.h = 300;
            big.pixels.assign(300u * 300u, 90);
            one.insert(aver::ui::UiGlyphKey{1, 16, 1}, big, 1);
            r->setTextAtlas(&one);
            MockContext first;
            r->prePass(first);
            const usize t = f.textures.size();
            one.insert(aver::ui::UiGlyphKey{1, 16, 2}, big, 2);   // recycles page 0
            check(one.pageDirty(0) && one.pageGeneration(0) == 1, "recycling dirties the page");
            MockContext ctx;
            r->prePass(ctx);
            check(ctx.count(Call::Kind::CopyBufferToTexture) == 1 && f.textures.size() == t, "it uploads again into the existing texture");
            r->setTextAtlas(&atlas);
        }
        {   // the page size changes: the old texture is replaced
            aver::ui::UiGlyphAtlas bigger({512, 2});
            aver::ui::UiGlyphBitmap b2;
            b2.w = 8;
            b2.h = 8;
            b2.pixels.assign(64, 255);
            bigger.insert(aver::ui::UiGlyphKey{1, 16, 1}, b2, 1);
            r->setTextAtlas(&bigger);
            const usize before = f.destroyedTextures.size();
            const usize t = f.textures.size();
            MockContext ctx;
            r->prePass(ctx);
            check(f.textures.size() == t + 1 && f.textures.back().width == 512, "a page of a new size gets a new texture");
            check(f.destroyedTextures.size() == before + 1, "and the old one is destroyed");
            r->setTextAtlas(&atlas);
        }

        // tokens in the draw list
        const rhi::TextureHandle pageTex = r->pageTexture(0);
        check(pageTex != 0 && r->pageTexture(5) == 0, "pageTexture answers for pages that have one");
        {
            aver::ui::UiDrawList dl;
            dl.addRect(0, 0, 10, 10, 0xFFFFFFFF);                                                                   // white
            dl.addTexturedRect(0, 0, 10, 10, aver::render::ui::UiRenderer::atlasToken(0), 0, 0, 1, 1, 0xFFFFFFFF);   // the page
            dl.addTexturedRect(0, 0, 10, 10, 7, 0, 0, 1, 1, 0xFFFFFFFF);                                            // a plain handle
            dl.addTexturedRect(0, 0, 10, 10, aver::render::ui::UiRenderer::atlasToken(9), 0, 0, 1, 1, 0xFFFFFFFF);   // no such page
            MockContext ctx;
            const usize setsBefore = f.sets.size();
            r->submit(dl);
            r->overlayPass(ctx, 1280, 720);
            check(ctx.count(Call::Kind::DrawIndexed) == 3, "the draw whose page has no texture is skipped; white, page and plain handle draw");
            bool pageBound = false;
            for (usize i = setsBefore; i < f.srvSlot0.size(); ++i) if (f.srvSlot0[i] == pageTex) pageBound = true;
            check(pageBound, "the token is bound as the page GPU texture");
            check(aver::render::ui::UiRenderer::atlasToken(3) == (aver::ui::kUiAtlasTokenBit | 3) && (aver::ui::kUiAtlasTokenBit & 0xFFFFFFFFull) == 0,
                  "a token sets bit 63, which a 32-bit texture handle can never reach");
        }
        r->setTextAtlas(nullptr);
        {
            MockContext ctx;
            r->prePass(ctx);
            check(ctx.calls.empty(), "setTextAtlas(nullptr) is safe and prePass does nothing again");
        }
    }

    AVER_INFO("=== a UI image (a content path's texture) ===");
    {
        MockFactory& f = dev.factory;
        ImageData img;
        img.width = 4; img.height = 4; img.channels = 4;
        img.pixels.assign(4 * 4 * 4, 0);
        for (usize i = 0; i < img.pixels.size(); i += 4) { img.pixels[i] = 200; img.pixels[i + 1] = 100; img.pixels[i + 2] = 50; img.pixels[i + 3] = 128; }
        aver::render::ui::UiImageLevels lv;
        check(aver::render::ui::buildUiImageLevels(img, lv), "the image builds");
        const usize before = f.textures.size();
        const rhi::TextureHandle tex = aver::render::ui::uploadUiImageLevels(f, lv, "Textures/ui/a.png");
        check(tex != 0 && f.textures.size() == before + 1, "one texture is created");
        const rhi::TextureDesc& d = f.textures.back();
        check(d.format == rhi::Format::RGBA8Unorm, "UNORM, not sRGB: the UI blends in display space");
        check(d.width == 4 && d.height == 4 && d.mips == 3 && d.initialDataCount == 3, "4x4 with a full chain (4, 2, 1) uploaded at creation");
        check(d.initialRowPitch == 0 && d.initialState == rhi::ResourceState::ShaderResource, "tightly packed, ready to sample");
        check(f.textureNames.back() == "Textures/ui/a.png", "named after its path");
        check(lv.levels[0][0] == 100 && lv.levels[0][3] == 128, "the texels it was given are premultiplied (200 * 128 / 255 = 100)");

        // Drawn once, then forgotten before it goes: the way the host's destroy callback uses the renderer.
        aver::ui::UiDrawList dl;
        dl.addTexturedRect(0, 0, 10, 10, aver::render::ui::UiRenderer::textureId(tex), 0, 0, 1, 1, 0xFFFFFFFF);
        MockContext ctx;
        r->submit(dl);
        r->overlayPass(ctx, 1280, 720);
        check(ctx.count(Call::Kind::DrawIndexed) == 1, "a UI image draws like any texture handle");
        const usize destroyedSets = f.destroyedBindingSets;
        r->dropBinding(tex);
        check(f.destroyedBindingSets == destroyedSets + 1, "dropBinding forgets its binding set");
        r->submit(aver::ui::UiDrawList{});
        MockContext idle;
        r->overlayPass(idle, 1280, 720);
        check(idle.count(Call::Kind::DrawIndexed) == 0, "an emptied submit draws nothing, so a destroyed image is never named");

        // A list submitted before the texture went (a sub-viewport rebuilt or stopped later in the frame) still names it.
        f.destroyTexture(tex);
        const usize setsBefore = f.sets.size();
        r->submit(dl);
        MockContext stale;
        r->overlayPass(stale, 1280, 720);
        check(stale.count(Call::Kind::DrawIndexed) == 0 && f.sets.size() == setsBefore,
              "a draw naming a destroyed texture is skipped, with no binding set made for it");
    }

    AVER_INFO("=== P41: sampler and blend are other pipelines, built when asked for ===");
    {
        MockFactory& f = dev.factory;
        const usize pipesBefore = f.pipelines.size();
        const usize shadersBefore = f.shaders.size();
        aver::ui::UiDrawList dl;
        dl.addRect(0, 0, 10, 10, 0xFFFFFFFF);
        MockContext plain;
        r->submit(dl);
        r->overlayPass(plain, 1280, 720);
        check(f.pipelines.size() == pipesBefore && f.shaders.size() == shadersBefore, "a default-state list builds nothing new");
        check(plain.count(Call::Kind::Pipeline) == 1, "and sets the one pipeline once");

        dl.clear();
        dl.addRect(0, 0, 10, 10, 0xFFFFFFFF);
        dl.setSampler(aver::ui::UiFilter::Nearest, aver::ui::UiAddress::Repeat);
        dl.addRect(20, 0, 10, 10, 0xFFFFFFFF);
        dl.setBlend(aver::ui::UiBlend::Add);
        dl.addRect(40, 0, 10, 10, 0xFFFFFFFF);
        dl.setSampler(aver::ui::UiFilter::Linear, aver::ui::UiAddress::Clamp);
        dl.setBlend(aver::ui::UiBlend::Mul);
        dl.addRect(60, 0, 10, 10, 0xFFFFFFFF);
        MockContext ctx;
        r->submit(dl);
        r->overlayPass(ctx, 1280, 720);
        check(f.pipelines.size() == pipesBefore + 3, "three new pipelines (nearest+repeat, add, multiply)");
        const rhi::GraphicsPipelineDesc& nr = f.pipelines[pipesBefore];
        check(nr.layout.samplers[0].filter == rhi::Filter::Point && nr.layout.samplers[0].address == rhi::AddressMode::Wrap &&
              nr.blend == rhi::BlendMode::PremultipliedAlpha, "the first bakes a point, wrapping sampler and keeps the premultiplied blend");
        check(f.pipelines[pipesBefore + 1].blend == rhi::BlendMode::Additive, "add is the additive blend");
        check(f.pipelines[pipesBefore + 2].blend == rhi::BlendMode::Multiplicative, "multiply is the multiplicative blend");
        check(f.shaders.size() == shadersBefore + 1 && f.shaders.back().entry && std::string(f.shaders.back().entry) == "UiPSMul",
              "and multiply compiles its own pixel entry, once");
        check(f.pipelines[pipesBefore + 2].ps == f.shaders.size() && f.pipelines[pipesBefore].ps == 2, "the other pipelines keep UiPS");
        check(ctx.count(Call::Kind::DrawIndexed) == 4, "four draws");
        check(ctx.count(Call::Kind::Pipeline) == 4, "the pipeline is switched at each state change");
        check(ctx.count(Call::Kind::Constants) == 4, "and the root constants are set again after each switch");

        MockContext again;
        r->submit(dl);
        r->overlayPass(again, 1280, 720);
        check(f.pipelines.size() == pipesBefore + 3 && f.shaders.size() == shadersBefore + 1, "a second frame builds nothing");
    }

    AVER_INFO("=== E6: world text (labels in the scene's transparent pass) ===");
    {
        MockFactory& f = dev.factory;
        aver::ui::UiGlyphAtlas world({512, 2});
        aver::ui::UiGlyphBitmap bmp;
        bmp.w = bmp.h = 8; bmp.offX = 0; bmp.offY = -8;
        bmp.pixels.assign(64, 255);
        world.insert(aver::ui::UiGlyphKey{1, 20, 1}, bmp, 1);
        r->setWorldAtlas(&world);
        MockContext pre;
        r->prePass(pre);
        const rhi::TextureHandle pageTex = r->worldPageTexture(0);
        check(pageTex != 0 && pre.count(Call::Kind::CopyBufferToTexture) >= 1, "the world atlas page is uploaded by prePass");
        check(f.textureNames.back() == "Aver.UI world glyph page 0", "as its own texture, named for the world atlas");
        check(world.pageDirty(0) == false, "and marked uploaded");

        const f32 eye[3] = {0, 0, 0}, target[3] = {10, 0, 0}, upv[3] = {0, 0, 1};
        const render::ui::WorldTextCamera cam = testcam::makeCamera(eye, target, upv, 1.0f, 16.0f / 9.0f, 720);
        for (int i = 0; i < 16; ++i) { dev.vp[i] = cam.viewProj[i]; dev.ivp[i] = cam.invViewProjRel[i]; }
        dev.haveCamera = true;
        r->onRenderTargetsChanged(1, rhi::Format::RGBA16F, rhi::Format::D32Float, 1280, 720);

        const auto quad = [](aver::ui::UiDrawList& dl, f32 x, u32 flags, u32 page, f32 alphaCut, f32 intensity) {
            const aver::ui::UiVertex v[4] = {{0, 0, 0, 0, 0xFFFFFFFF}, {1, 0, 1, 0, 0xFFFFFFFF}, {1, 1, 1, 1, 0xFFFFFFFF}, {0, 1, 0, 1, 0xFFFFFFFF}};
            const u32 idx[6] = {0, 1, 2, 0, 2, 3};
            const aver::ui::UiWorldTextRun run{0, 6, page};
            aver::ui::UiWorldText h;
            h.world[12] = x;
            h.flags = flags;
            h.alphaCut = alphaCut;
            h.intensity = intensity;
            return dl.addWorldText(h, v, 4, idx, 6, &run, 1);
        };
        const auto floatsOf = [&](usize bytes) -> const BufferRecord* {
            // The buffer the pass wrote `bytes` of: found by size, the pass being the only writer of that size here.
            for (auto it = f.buffers.rbegin(); it != f.buffers.rend(); ++it)
                if (it->contents.size() == bytes) return &*it;
            return nullptr;
        };

        aver::ui::UiDrawList dl;
        check(quad(dl, 10.0f, 0, 0, 0.2f, 2.0f), "a label is added to the list");
        const usize shadersBefore = f.shaders.size(), pipesBefore = f.pipelines.size();
        r->submit(dl);
        MockContext ctx;
        r->transparentPass(ctx);
        check(f.shaders.size() == shadersBefore + 2 && std::string(f.shaders[shadersBefore].entry) == "WorldTextVS" &&
              std::string(f.shaders[shadersBefore + 1].entry) == "WorldTextPS", "the world shaders are compiled on first use, not at init");
        check(f.pipelines.size() == pipesBefore + 1, "one pipeline");
        const rhi::GraphicsPipelineDesc& pd = f.pipelines.back();
        check(pd.blend == rhi::BlendMode::PremultipliedAlpha && pd.depth.test && !pd.depth.write && pd.cull == rhi::CullMode::None,
              "premultiplied, depth tested, no depth write, two sided");
        check(pd.renderTargets[0] == rhi::Format::RGBA16F && pd.depthFormat == rhi::Format::D32Float && pd.sampleCount == 1,
              "built for the scene's colour and depth formats");
        check(pd.layout.constantDwords[3] == 20 && pd.layout.srvCount == 1 && pd.layout.samplerCount == 1, "20 root constants at b3, one texture, one sampler");
        check(pd.vertexLayout.stride == 24 && pd.vertexLayout.attribCount == 4, "a 24-byte vertex of four attributes");
        check(pd.vertexLayout.attribs[1].semanticIndex == 1 && pd.vertexLayout.attribs[1].format == rhi::Format::R32Float && pd.vertexLayout.attribs[2].offset == 12 &&
              pd.vertexLayout.attribs[3].offset == 20, "position is split 2 + 1 as the particle mesh path, then uv at 12 and colour at 20");
        check(ctx.count(Call::Kind::DrawIndexed) == 1 && ctx.ofKind(Call::Kind::DrawIndexed)[0].a == 6, "one draw of six indices");
        check(ctx.count(Call::Kind::Pipeline) == 1 && ctx.count(Call::Kind::Constants) == 1 && ctx.count(Call::Kind::BindingSet) == 1, "one pipeline, one constants block, one binding");
        check(ctx.lastConstants.size() == 20 && ctx.lastConstants[16] == 0.2f && ctx.lastConstants[17] == 2.0f && ctx.lastConstants[18] == 0.0f,
              "the constants are the view-projection then alpha cut 0.2, intensity 2, not SDF");
        bool vpSame = true;
        for (int i = 0; i < 16; ++i) vpSame = vpSame && ctx.lastConstants[i] == cam.viewProj[i];
        check(vpSame, "the first sixteen are the camera's view-projection");
        const BufferRecord* vbRec = floatsOf(4 * 24);
        check(vbRec != nullptr, "four 24-byte vertices were written");
        if (vbRec) {
            const f32* p = reinterpret_cast<const f32*>(vbRec->contents.data());
            check(p[12] == 11.0f && p[13] == 1.0f && p[14] == 0.0f, "the third vertex is the plane's (1, 1) placed at the label's origin x = 10");
        }
        const BufferRecord* ibRec = floatsOf(6 * 4);
        check(ibRec != nullptr && reinterpret_cast<const u32*>(ibRec->contents.data())[2] == 2, "and the six indices");
        check(r->worldTextsDrawn() == 1 && r->worldTextsSkipped() == 0, "counted as drawn");

        // Ray-driven: skipped unless the host opts in.
        r->setRayDrivenActive(true);
        MockContext rd;
        r->transparentPass(rd);
        check(rd.calls.empty() && r->worldTextsSkipped() == 1, "in ray-driven mode a label draws nothing and is counted as skipped");
        r->setWorldTextInRayDriven(true);
        MockContext rd2;
        r->transparentPass(rd2);
        check(rd2.count(Call::Kind::DrawIndexed) == 1, "unless the host opted in");
        r->setRayDrivenActive(false);
        r->setWorldTextInRayDriven(false);
        r->setWorldTextEnabled(false);
        MockContext off;
        r->transparentPass(off);
        check(off.calls.empty(), "and the whole feature can be switched off");
        r->setWorldTextEnabled(true);

        // No camera: nothing is guessed.
        dev.haveCamera = false;
        MockContext nocam;
        r->transparentPass(nocam);
        check(nocam.calls.empty(), "with no camera nothing is drawn");
        dev.haveCamera = true;

        // The flags choose the pipeline.
        aver::ui::UiDrawList flagged;
        quad(flagged, 10.0f, aver::ui::kWorldTextNoDepth | aver::ui::kWorldTextAdditive | aver::ui::kWorldTextOneSided, 0, 0, 1);
        const usize pipes1 = f.pipelines.size();
        r->submit(flagged);
        MockContext fc;
        r->transparentPass(fc);
        check(f.pipelines.size() == pipes1 + 1, "a different state is another pipeline");
        const rhi::GraphicsPipelineDesc& fd = f.pipelines.back();
        check(!fd.depth.test && fd.blend == rhi::BlendMode::Additive && fd.cull == rhi::CullMode::Back, "no depth test, additive, back faces culled");
        check(fd.depthFormat == rhi::Format::D32Float, "the depth format still matches the bound target");
        MockContext fc2;
        r->transparentPass(fc2);
        check(f.pipelines.size() == pipes1 + 1, "and built once");

        // Far to near.
        aver::ui::UiDrawList two;
        quad(two, 10.0f, 0, 0, 0, 1);
        quad(two, 30.0f, 0, 0, 0, 1);
        r->submit(two);
        MockContext sc;
        r->transparentPass(sc);
        const BufferRecord* two8 = floatsOf(8 * 24);
        check(two8 != nullptr && reinterpret_cast<const f32*>(two8->contents.data())[0] == 30.0f &&
              reinterpret_cast<const f32*>(two8->contents.data())[4 * 6] == 10.0f, "two labels are written far first (30 m, then 10 m)");
        check(sc.count(Call::Kind::DrawIndexed) == 2, "and drawn in that order");

        // Pages and indices are clamped.
        aver::ui::UiDrawList badPage;
        quad(badPage, 10.0f, 0, 5, 0, 1);
        r->submit(badPage);
        MockContext bp;
        r->transparentPass(bp);
        check(bp.count(Call::Kind::DrawIndexed) == 0 && r->worldTextsSkipped() == 1, "a run on a page the world atlas does not have is not drawn");
        aver::ui::UiDrawList noTex;
        quad(noTex, 10.0f, 0, 1, 0, 1);   // page 1 exists in the cap but the atlas has one page
        r->submit(noTex);
        MockContext nt;
        r->transparentPass(nt);
        check(nt.count(Call::Kind::DrawIndexed) == 0, "nor on a page with no texture yet");

        // A billboard behind the camera and one in front.
        aver::ui::UiDrawList bb;
        quad(bb, -10.0f, aver::ui::kWorldTextBillboard, 0, 0, 1);
        quad(bb, 10.0f, aver::ui::kWorldTextBillboard, 0, 0, 1);
        r->submit(bb);
        MockContext bc;
        r->transparentPass(bc);
        check(bc.count(Call::Kind::DrawIndexed) == 1 && r->worldTextsDrawn() == 1 && r->worldTextsSkipped() == 1, "a billboard behind the camera is skipped, the one in front drawn");

        // A distance-field atlas tells the shader.
        world.setSdf(true);
        r->submit(dl);
        MockContext sdfc;
        r->transparentPass(sdfc);
        check(sdfc.lastConstants.size() == 20 && sdfc.lastConstants[18] == 1.0f, "a distance-field world atlas sets the SDF constant");
        world.setSdf(false);

        // The renderer does not keep the atlas: nulling it before teardown stops page uploads and draws.
        r->setWorldAtlas(nullptr);
        MockContext gone;
        r->transparentPass(gone);
        check(gone.count(Call::Kind::DrawIndexed) == 0, "with the atlas withdrawn no label draws");
        r->setWorldAtlas(&world);
        r->submit(aver::ui::UiDrawList{});
    }

    delete r;
    // The atlas above outlived `delete r` only because the renderer held a pointer, never an owner.

    if (g_failures == 0) AVER_INFO("=== all UI renderer tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
