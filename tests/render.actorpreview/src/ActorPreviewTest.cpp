// The actor preview, against a RECORDING DEVICE rather than a GPU.
//
// The preview is the piece of the actor editor that decides what an author SEES, and almost every
// way it can be wrong is invisible in a still frame: a camera published at the wrong register works
// until the next pipeline change; a transposed matrix puts every part in a plausible wrong place; a
// missing barrier is a driver's problem to notice, not a picture's.
//
// Same shape as UiRenderTest, and possible for the same reason: this feature talks to the generic
// RHI and never to a backend, so a test can be the device.
#include "aver/render/preview/ActorPreview.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

namespace {

struct MockFactory final : public rhi::IResourceFactory {
    std::vector<rhi::TextureDesc> textures;
    std::vector<rhi::ShaderDesc> shaders;
    std::vector<rhi::GraphicsPipelineDesc> pipelines;
    u32 waited = 0, destroyedTextures = 0;

    rhi::TextureHandle createTexture(const rhi::TextureDesc& d) override {
        textures.push_back(d); return static_cast<rhi::TextureHandle>(textures.size());
    }
    rhi::BufferHandle createBuffer(const rhi::BufferDesc&) override { return 1; }
    rhi::ShaderHandle createShader(const rhi::ShaderDesc& d) override {
        shaders.push_back(d); return static_cast<rhi::ShaderHandle>(shaders.size());
    }
    rhi::PipelineHandle createGraphicsPipeline(const rhi::GraphicsPipelineDesc& d) override {
        pipelines.push_back(d); return static_cast<rhi::PipelineHandle>(pipelines.size());
    }
    rhi::PipelineHandle createComputePipeline(const rhi::ComputePipelineDesc&) override { return 0; }
    rhi::BindingSetHandle createBindingSet(const rhi::BindingSetDesc&) override { return 1; }
    rhi::BlasHandle createBlas(rhi::MeshHandle) override { return 0; }
    rhi::TlasHandle createTlas(u32) override { return 0; }
    void destroyTexture(rhi::TextureHandle) override { ++destroyedTextures; }
    void destroyBuffer(rhi::BufferHandle) override {}
    void destroyShader(rhi::ShaderHandle) override {}
    void destroyPipeline(rhi::PipelineHandle) override {}
    void destroyBindingSet(rhi::BindingSetHandle) override {}
    void setSrv(rhi::BindingSetHandle, u32, rhi::TextureHandle, u32) override {}
    void setUav(rhi::BindingSetHandle, u32, rhi::TextureHandle, u32) override {}
    void setSrvTlas(rhi::BindingSetHandle, u32, rhi::TlasHandle) override {}
    bool writeBuffer(rhi::BufferHandle, const void*, u64, u64) override { return true; }
    bool textureInfo(rhi::TextureHandle, rhi::TextureDesc&) const override { return false; }
    void waitIdle() override { ++waited; }
};

struct Call {
    enum class Kind { Pipeline, Viewport, Scissor, Targets, ClearDepth, Constants, ConstantBuffer,
                      DrawMesh, Barrier };
    Kind kind;
    u32 a = 0, b = 0, c = 0, d = 0;
    std::vector<f32> payload;   // for Constants / ConstantBuffer
};

struct MockContext final : public rhi::IRenderContext {
    std::vector<Call> calls;

    void setPipeline(rhi::PipelineHandle p) override { calls.push_back({Call::Kind::Pipeline, p}); }
    void setViewport(u32 x, u32 y, u32 w, u32 h) override { calls.push_back({Call::Kind::Viewport, x, y, w, h}); }
    void setScissor(u32 x, u32 y, u32 w, u32 h) override { calls.push_back({Call::Kind::Scissor, x, y, w, h}); }
    void setRenderTargets(const rhi::TextureHandle* c, u32 n, rhi::TextureHandle depth) override {
        calls.push_back({Call::Kind::Targets, n ? c[0] : 0u, n, depth});
    }
    void clearDepth(rhi::TextureHandle t, f32) override { calls.push_back({Call::Kind::ClearDepth, t}); }
    void setBindingSet(rhi::BindingSetHandle, u32) override {}
    void setConstants(u32 slot, const void* data, u32 dwords) override {
        Call c{Call::Kind::Constants, slot, dwords};
        c.payload.resize(dwords);
        if (data && dwords) std::memcpy(c.payload.data(), data, dwords * sizeof(f32));
        calls.push_back(std::move(c));
    }
    void setConstantBuffer(u32 slot, const void* data, u32 bytes) override {
        Call c{Call::Kind::ConstantBuffer, slot, bytes};
        c.payload.resize(bytes / sizeof(f32));
        if (data && bytes) std::memcpy(c.payload.data(), data, bytes);
        calls.push_back(std::move(c));
    }
    void drawMesh(rhi::MeshHandle m) override { calls.push_back({Call::Kind::DrawMesh, m}); }
    void dispatchMeshFor(rhi::MeshHandle) override {}
    void dispatch(u32, u32, u32) override {}
    void drawFullscreen() override {}
    void setVertexBuffer(rhi::BufferHandle, u32) override {}
    void setIndexBuffer(rhi::BufferHandle, rhi::Format) override {}
    void drawIndexed(u32, u32, i32) override {}
    void buildBlas(rhi::BlasHandle) override {}
    void buildTlas(rhi::TlasHandle, const rhi::TlasInstance*, u32) override {}
    void textureBarrier(rhi::TextureHandle t, rhi::ResourceState from, rhi::ResourceState to, u32) override {
        calls.push_back({Call::Kind::Barrier, t, static_cast<u32>(from), static_cast<u32>(to)});
    }
    void bufferBarrier(rhi::BufferHandle, rhi::ResourceState, rhi::ResourceState) override {}
    void uavBarrierTexture(rhi::TextureHandle) override {}
    void uavBarrierBuffer(rhi::BufferHandle) override {}

    std::vector<Call> ofKind(Call::Kind k) const {
        std::vector<Call> out;
        for (const Call& c : calls) if (c.kind == k) out.push_back(c);
        return out;
    }
    usize count(Call::Kind k) const { return ofKind(k).size(); }
};

struct MockDevice final : public rhi::IDevice {
    MockFactory factory;
    u32 uiIds = 0;
    rhi::Backend backend() const override { return rhi::Backend::Null; }
    const char* adapterName() const override { return "recording device"; }
    rhi::IResourceFactory* resources() override { return &factory; }
    rhi::ISwapchain* createSwapchain(const rhi::SwapchainDesc&) override { return nullptr; }
    void beginFrame() override {}
    void endFrame() override {}
    u64 uiTextureId(rhi::TextureHandle) override { return ++uiIds + 1000; }
};

// A device with no GPU support at all, to prove the feature declines rather than half-initialising.
struct NullDevice final : public rhi::IDevice {
    rhi::Backend backend() const override { return rhi::Backend::Null; }
    const char* adapterName() const override { return "no gpu"; }
    rhi::ISwapchain* createSwapchain(const rhi::SwapchainDesc&) override { return nullptr; }
    void beginFrame() override {}
    void endFrame() override {}
};

} // namespace

int main() {
    using namespace aver::render::preview;

    AVER_INFO("=== declining without a GPU ===");
    {
        NullDevice dead;
        check(ActorPreview::create(dead) == nullptr,
              "a backend with no resource factory yields no preview, rather than a broken one");
    }

    MockDevice dev;
    ActorPreview* p = ActorPreview::create(dev, 1024);
    if (!p) { AVER_ERROR("=== create declined against the recording device ==="); return 1; }

    AVER_INFO("=== what init creates ===");
    {
        const MockFactory& f = dev.factory;
        check(f.textures.size() == 2, "two targets: colour and depth");
        check(f.textures[0].width == 1024 && f.textures[0].height == 1024, "square, at the asked size");
        // NOT sRGB: the pixel shader gamma-encodes itself, so an sRGB view would encode twice and
        // wash the whole preview out -- which reads as "the preview lighting is wrong".
        check(f.textures[0].format == rhi::Format::RGBA8Unorm, "the colour target is UNORM, not sRGB");
        check(aver::rhi::any(f.textures[0].bind, rhi::ResourceBind::RenderTarget) &&
              aver::rhi::any(f.textures[0].bind, rhi::ResourceBind::ShaderResource),
              "and is both drawn into and sampled");
        check(f.textures[0].initialState == rhi::ResourceState::ShaderResource,
              "created in the state every frame LEAVES it in, so the first barrier is honest");
        check(f.textures[1].format == rhi::Format::D32Float, "a real depth buffer, not a shared one");

        check(f.pipelines.size() == 1, "one pipeline");
        const rhi::GraphicsPipelineDesc& gp = f.pipelines[0];
        check(gp.depth.test && gp.depth.write, "depth tested and written: parts occlude each other");
        check(gp.cull == rhi::CullMode::Back, "back faces culled");
        check(gp.sampleCount == 1, "never multisampled -- the UI samples this, it is not resolved");
        check(gp.renderTargetCount == 1 && gp.renderTargets[0] == rhi::Format::RGBA8Unorm,
              "one target, matching what was created");
        check(gp.depthFormat == rhi::Format::D32Float, "and a depth format, or nothing would occlude");

        // THE REGISTER DECISION, asserted rather than trusted. b0 is the engine's block and the
        // backend rebinds it on every setPipeline, so a camera published there survives until the
        // next pipeline change and then silently becomes the level's camera.
        check(gp.layout.constantDwords[rhi::kEngineFrameConstantRegister] == 0,
              "slot 0 is left to the BACKEND -- a feature must never observe b0 unbound");
        check(gp.layout.constantDwords[rhi::kFeatureFrameConstantRegister] == 0,
              "the feature's camera is a root CBV at b4, not root constants");
        check(gp.layout.constantDwords[rhi::kObjectConstantRegister] == rhi::kObjectConstantDwords,
              "and b1 declares exactly the per-draw block the shared prelude does");

        check(f.shaders.size() == 2, "a vertex and a pixel shader");
        // Compiled as the TAIL of the shared prelude, so VSIn and the b0/b1 layouts have one owner.
        check(f.shaders[0].prelude != nullptr, "compiled against the shared prelude");
        const std::string hlsl = actorPreviewShaderSource();
        check(hlsl.find("register(b4)") != std::string::npos, "the camera cbuffer is at b4");
        check(hlsl.find("register(b0)") == std::string::npos,
              "and the preview declares nothing at b0");
        // averSkyAbove reads the sky out of b0, so a b4 preview cannot use it and must not pretend to.
        check(hlsl.find("averSkyAbove") == std::string::npos,
              "it writes its own backdrop rather than sampling a sky it cannot reach");

        check(p->uiTextureId() != 0, "the colour target is reachable from the UI");
        check(p->ready(), "and the feature reports ready");
    }

    AVER_INFO("=== an empty actor ===");
    {
        MockContext ctx;
        p->setDrawList({});
        p->prePass(ctx);
        // The pass still runs: it CLEARS. A preview that skipped the pass when the list was empty
        // would leave the last actor's image on screen after you deleted its models.
        check(ctx.count(Call::Kind::DrawMesh) == 0, "nothing is drawn");
        check(ctx.count(Call::Kind::ClearDepth) == 1, "but the depth target is still cleared");
        check(ctx.count(Call::Kind::Targets) == 1, "and the targets are still bound");
    }

    AVER_INFO("=== drawing the authored composition ===");
    {
        std::vector<PreviewDraw> draws;
        for (int i = 0; i < 3; ++i) {
            PreviewDraw d;
            d.mesh = static_cast<rhi::MeshHandle>(i + 1);
            d.world[12] = static_cast<f32>(i) * 100.0f;   // translation is the LAST ROW
            d.world[13] = 0.0f;
            d.world[14] = 20.0f;
            draws.push_back(d);
        }
        draws[1].selected = true;
        // An unresolved mesh: the row parsed, but nothing registered that path. It must draw NOTHING
        // rather than a fallback cube, because a fallback shape in a preview reads as the actor
        // actually having a cube in it.
        PreviewDraw missing;
        missing.mesh = 0;
        draws.push_back(missing);

        MockContext ctx;
        p->setDrawList(draws);
        p->prePass(ctx);

        check(ctx.count(Call::Kind::DrawMesh) == 3, "three meshes drawn, and the unresolved one skipped");
        check(ctx.count(Call::Kind::Pipeline) == 1, "one pipeline bind for the whole pass");

        const std::vector<Call> vp = ctx.ofKind(Call::Kind::Viewport);
        check(vp.size() == 1 && vp[0].c == 1024 && vp[0].d == 1024, "the viewport is the whole target");
        // Set explicitly, not inherited: the editor leaves the scissor on its dock rect, which would
        // silently clip this pass to wherever the 3D view happens to sit.
        const std::vector<Call> sc = ctx.ofKind(Call::Kind::Scissor);
        check(sc.size() == 1 && sc[0].c == 1024, "and the scissor is set rather than inherited");

        // The per-draw block: the world matrix arrives with the translation in the LAST ROW. A
        // transposed matrix would put every part in a plausible wrong place, and the picture would
        // still look like an actor.
        const std::vector<Call> obj = ctx.ofKind(Call::Kind::Constants);
        check(obj.size() == 3, "one per-draw block per drawn mesh");
        if (obj.size() == 3) {
            check(obj[0].a == rhi::kObjectConstantRegister, "written at b1");
            check(obj[0].b == rhi::kObjectConstantDwords, "and the WHOLE declared block, never part of it");
            check(obj[2].payload[12] == 200.0f && obj[2].payload[14] == 20.0f,
                  "the translation is in row 3, per the engine's row-vector convention");
        }

        // The camera block, republished per draw so the selection flag can change with it.
        const std::vector<Call> cam = ctx.ofKind(Call::Kind::ConstantBuffer);
        check(cam.size() == 3, "the camera block is published per draw");
        check(cam[0].a == rhi::kFeatureFrameConstantRegister, "at b4");
        if (cam.size() == 3) {
            // index 16..19 is eye, 20..23 key, 24..27 ambient; ambient.w is the highlight.
            check(cam[0].payload[27] == 0.0f, "an unselected model gets no highlight");
            check(cam[1].payload[27] > 0.0f, "the selected one does");
            check(cam[2].payload[27] == 0.0f, "and it does not leak onto the next draw");
        }

        // The barriers bracket the pass. Without the closing one the UI samples a render target,
        // which is undefined and which no picture would show as wrong.
        const std::vector<Call> bar = ctx.ofKind(Call::Kind::Barrier);
        check(bar.size() == 2, "exactly two barriers: in and out");
        if (bar.size() == 2) {
            check(bar[0].c == static_cast<u32>(rhi::ResourceState::RenderTarget),
                  "into RenderTarget at the top");
            check(bar[1].b == static_cast<u32>(rhi::ResourceState::RenderTarget) &&
                  bar[1].c == static_cast<u32>(rhi::ResourceState::ShaderResource),
                  "and back to ShaderResource at the bottom, so the UI may sample it");
        }
    }

    AVER_INFO("=== the camera ===");
    {
        PreviewCamera c;
        c.pitchDeg = 80.0f;
        c.addOrbit(0.0f, 40.0f);
        // Short of the pole, where the up vector flips and the view rolls over with no hysteresis.
        check(c.pitchDeg <= 85.0f, "pitch is clamped short of the pole");
        c.addOrbit(0.0f, -400.0f);
        check(c.pitchDeg >= -85.0f, "at both ends");

        // Multiplicative zoom, so a wheel notch moves the same PROPORTION at every scale: a step that
        // frames a 5 cm bolt would otherwise put a 20 m vehicle in the next county.
        c.distance = 1000.0f;
        c.addZoom(0.5f);
        check(c.distance == 500.0f, "zoom is proportional");
        c.addZoom(0.0001f);
        check(c.distance >= 5.0f, "and clamped so it cannot reach the pivot");

        // frameAll sizes to the placements, so an actor authored in centimetres and one authored in
        // metres both arrive on screen without anybody scrolling.
        std::vector<PreviewDraw> wide;
        for (int i = 0; i < 2; ++i) {
            PreviewDraw d; d.mesh = 1;
            d.world[12] = static_cast<f32>(i) * 2000.0f;
            wide.push_back(d);
        }
        p->setDrawList(wide);
        p->frameAll();
        check(p->camera().distance > 2000.0f, "a wide actor is framed from further back");
        check(std::fabs(p->camera().pivot[0] - 1000.0f) < 1.0f, "and the pivot is its centre");

        p->setDrawList({});
        p->frameAll();
        check(p->camera().distance > 0.0f, "framing an empty list does not divide by zero");
    }

    AVER_INFO("=== teardown ===");
    {
        const u32 waitedBefore = dev.factory.waited;
        delete p;
        // waitIdle BEFORE destroying a texture the UI could still be sampling. The validation layer
        // does not catch this one; it is a use-after-free in somebody else's draw list.
        check(dev.factory.waited > waitedBefore, "the GPU is drained before the targets go");
        check(dev.factory.destroyedTextures >= 2, "and both targets are released");
    }

    if (g_failures == 0) AVER_INFO("=== all actor preview tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
