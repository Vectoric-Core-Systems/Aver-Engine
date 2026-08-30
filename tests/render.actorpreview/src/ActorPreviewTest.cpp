// The actor preview, driven against a recording RHI device rather than a GPU.
#include "aver/render/preview/ActorPreview.hpp"
#include "aver/render/preview/PreviewMeshCache.hpp"
#include "aver/core/Log.hpp"

#if AVER_MODULE_PBR
#include "aver/pbr/MaterialGraphRegistry.hpp"
#include "aver/formats/OcGraph.hpp"
#endif

#include <cmath>
#include <cstring>
#include <string>
#include <utility>
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

// Resource factory that records every descriptor it is handed and hands back sequential handles.
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
    // The ray path's bindless table plays no part in these tests; refuse rather than pretend.
    rhi::BindlessTableHandle createBindlessTextureTable(u32) override { return 0; }
    void destroyBindlessTextureTable(rhi::BindlessTableHandle) override {}
    bool setBindlessTexture(rhi::BindlessTableHandle, u32, rhi::TextureHandle) override { return false; }
    u32  bindlessTableCapacity(rhi::BindlessTableHandle) const override { return 0; }
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
    void setSrvBuffer(rhi::BindingSetHandle, u32, rhi::BufferHandle, u32, u32, u32) override {}
    void setUavBuffer(rhi::BindingSetHandle, u32, rhi::BufferHandle, u32, u32, u32) override {}
    bool writeBuffer(rhi::BufferHandle, const void*, u64, u64) override { return true; }
    bool readBuffer(rhi::BufferHandle, void*, u64, u64) override { return true; }
    bool textureInfo(rhi::TextureHandle, rhi::TextureDesc&) const override { return false; }
    void waitIdle() override { ++waited; }
};

// One recorded render-context call: its kind, up to four scalars, and any constant payload.
struct Call {
    enum class Kind { Pipeline, Viewport, Scissor, Targets, ClearDepth, ClearColor, Constants,
                      ConstantBuffer, DrawMesh, Barrier };
    Kind kind;
    u32 a = 0, b = 0, c = 0, d = 0;
    std::vector<f32> payload;   // for Constants / ConstantBuffer
};

// Render context that records the call stream instead of submitting it.
struct MockContext final : public rhi::IRenderContext {
    std::vector<Call> calls;

    void setPipeline(rhi::PipelineHandle p) override { calls.push_back({Call::Kind::Pipeline, p}); }
    void setViewport(u32 x, u32 y, u32 w, u32 h) override { calls.push_back({Call::Kind::Viewport, x, y, w, h}); }
    void setScissor(u32 x, u32 y, u32 w, u32 h) override { calls.push_back({Call::Kind::Scissor, x, y, w, h}); }
    void setRenderTargets(const rhi::TextureHandle* c, u32 n, rhi::TextureHandle depth) override {
        calls.push_back({Call::Kind::Targets, n ? c[0] : 0u, n, depth});
    }
    void clearDepth(rhi::TextureHandle t, f32) override { calls.push_back({Call::Kind::ClearDepth, t}); }
    void clearColor(rhi::TextureHandle t, const f32[4]) override { calls.push_back({Call::Kind::ClearColor, t}); }
    void setBindingSet(rhi::BindingSetHandle, u32) override {}
    void setBindlessTable(rhi::BindlessTableHandle) override {}
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
    void copyBuffer(rhi::BufferHandle, rhi::BufferHandle, u64, u64, u64) override {}
    void copyTexture(rhi::TextureHandle, rhi::TextureHandle) override {}
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

    // Every recorded call of one kind, in order.
    std::vector<Call> ofKind(Call::Kind k) const {
        std::vector<Call> out;
        for (const Call& c : calls) if (c.kind == k) out.push_back(c);
        return out;
    }
    usize count(Call::Kind k) const { return ofKind(k).size(); }
};

// Device that exposes the recording factory and hands out fresh UI texture ids.
struct MockDevice final : public rhi::IDevice {
    MockFactory factory;
    u32 uiIds = 0;
    // Every mesh upload, so a cache test can ask what actually reached the GPU rather than only what
    // the cache returned. IDevice::createMesh defaults to 0, which would make "the handle is stable"
    // trivially true and hide the very failure this records.
    std::vector<std::pair<u32, u32>> meshUploads;   // (vertexCount, indexCount) in call order
    rhi::Backend backend() const override { return rhi::Backend::Null; }
    const char* adapterName() const override { return "recording device"; }
    rhi::IResourceFactory* resources() override { return &factory; }
    rhi::ISwapchain* createSwapchain(const rhi::SwapchainDesc&) override { return nullptr; }
    void beginFrame() override {}
    void endFrame() override {}
    u64 uiTextureId(rhi::TextureHandle) override { return ++uiIds + 1000; }
    rhi::MeshHandle createMesh(const rhi::MeshVertex*, u32 vertexCount,
                               const u32*, u32 indexCount) override {
        meshUploads.emplace_back(vertexCount, indexCount);
        return static_cast<rhi::MeshHandle>(meshUploads.size());
    }
};

// Device with no resource factory at all, so the preview must decline.
struct NullDevice final : public rhi::IDevice {
    rhi::Backend backend() const override { return rhi::Backend::Null; }
    const char* adapterName() const override { return "no gpu"; }
    rhi::ISwapchain* createSwapchain(const rhi::SwapchainDesc&) override { return nullptr; }
    void beginFrame() override {}
    void endFrame() override {}
};

#if AVER_MODULE_PBR
// Copied from tests/formats/src/MaterialGraphTest.cpp's own node()/addPin()/link() -- see its header
// comment for why a hand-built OcGraphData, not a parsed .ocgraph fixture, is the right way to drive
// this. Copied rather than shared because that test lives in a different module (tests/formats) and
// this one must not link it just to borrow three small functions.
fmt::OcGraphNode node(const char* id, const char* type) {
    fmt::OcGraphNode n;
    n.id = id;
    n.type = type;
    return n;
}
void addPin(fmt::OcGraphNode& n, const char* name, const char* type, bool out, const char* def = "") {
    fmt::OcGraphPin p;
    p.name = name;
    p.type = type;
    p.isOutput = out;
    p.defaultValue = def;
    n.pins.push_back(p);
}
void link(fmt::OcGraphData& g, const char* sn, const char* sp, const char* dn, const char* dp) {
    fmt::OcGraphLink l;
    l.sourceNode = sn;
    l.sourcePin = sp;
    l.destNode = dn;
    l.destPin = dp;
    g.links.push_back(l);
}

// The two-node graph the material-path checks below are proved on: a constant colour into
// BaseColor, identical in shape to MaterialGraphTest.cpp's own flatColorGraph().
fmt::OcGraphData flatColorGraph() {
    fmt::OcGraphData g;
    g.name = "M_ActorPreviewTest";
    g.domain = "material";
    fmt::OcGraphNode c = node("colour", "ConstFloat3");
    addPin(c, "value", "float3", true, "0.85,0.16,0.10");
    g.nodes.push_back(c);
    fmt::OcGraphNode out = node("out", "MaterialOutput");
    addPin(out, "BaseColor", "float3", false);
    g.nodes.push_back(out);
    link(g, "colour", "value", "out", "BaseColor");
    return g;
}
#endif

} // namespace

// Runs every actor preview check. Returns 1 if any failed.
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

        check(gp.layout.constantDwords[rhi::kEngineFrameConstantRegister] == 0,
              "slot 0 is left to the BACKEND -- a feature must never observe b0 unbound");
        check(gp.layout.constantDwords[rhi::kFeatureFrameConstantRegister] == 0,
              "the feature's camera is a root CBV at b4, not root constants");
        check(gp.layout.constantDwords[rhi::kObjectConstantRegister] == rhi::kObjectConstantDwords,
              "and b1 declares exactly the per-draw block the shared prelude does");

        check(f.shaders.size() == 2, "a vertex and a pixel shader");
        check(f.shaders[0].prelude != nullptr, "compiled against the shared prelude");
        const std::string hlsl = actorPreviewShaderSource();
        check(hlsl.find("register(b4)") != std::string::npos, "the camera cbuffer is at b4");
        check(hlsl.find("register(b0)") == std::string::npos,
              "and the preview declares nothing at b0");
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
        check(ctx.count(Call::Kind::DrawMesh) == 0, "nothing is drawn");
        check(ctx.count(Call::Kind::ClearDepth) == 1, "but the depth target is still cleared");
        // THE REGRESSION: the colour target used to have no clear at all, so an actor with holes,
        // or one shrunk by the scale gizmo, kept every earlier frame's pixels wherever nothing drew
        // over them -- an empty actor is the starkest case, since NOTHING draws and the old target
        // would have shown whatever was there from the frame before.
        check(ctx.count(Call::Kind::ClearColor) == 1, "and the colour target is cleared too");
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
        const std::vector<Call> sc = ctx.ofKind(Call::Kind::Scissor);
        check(sc.size() == 1 && sc[0].c == 1024, "and the scissor is set rather than inherited");

        const std::vector<Call> obj = ctx.ofKind(Call::Kind::Constants);
        check(obj.size() == 3, "one per-draw block per drawn mesh");
        if (obj.size() == 3) {
            check(obj[0].a == rhi::kObjectConstantRegister, "written at b1");
            check(obj[0].b == rhi::kObjectConstantDwords, "and the WHOLE declared block, never part of it");
            check(obj[2].payload[12] == 200.0f && obj[2].payload[14] == 20.0f,
                  "the translation is in row 3, per the engine's row-vector convention");
        }

        const std::vector<Call> cam = ctx.ofKind(Call::Kind::ConstantBuffer);
        check(cam.size() == 3, "the camera block is published per draw");
        check(cam[0].a == rhi::kFeatureFrameConstantRegister, "at b4");
        if (cam.size() == 3) {
            // index 16..19 is eye, 20..23 key, 24..27 ambient; ambient.w is the highlight.
            check(cam[0].payload[27] == 0.0f, "an unselected model gets no highlight");
            check(cam[1].payload[27] > 0.0f, "the selected one does");
            check(cam[2].payload[27] == 0.0f, "and it does not leak onto the next draw");
        }

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
        check(c.pitchDeg <= 85.0f, "pitch is clamped short of the pole");
        c.addOrbit(0.0f, -400.0f);
        check(c.pitchDeg >= -85.0f, "at both ends");

        c.distance = 1000.0f;
        c.addZoom(0.5f);
        check(c.distance == 500.0f, "zoom is proportional");
        c.addZoom(0.0001f);
        check(c.distance >= 5.0f, "and clamped so it cannot reach the pivot");

        {
            PreviewCamera p;
            p.yawDeg = 0.0f; p.pitchDeg = 0.0f; p.distance = 400.0f; p.fovDeg = 45.0f;
            p.pivot[0] = p.pivot[1] = p.pivot[2] = 0.0f;

            p.panPixels(100.0f, 0.0f, 1000.0f);
            check(p.pivot[1] < -1.0f, "drag right pans the pivot along -Y (scene follows the cursor)");
            check(std::fabs(p.pivot[2]) < 1e-3f, "a horizontal drag does not change height");
            check(std::fabs(p.pivot[0]) < 1e-3f, "and does not move along the view axis");

            PreviewCamera q;
            q.yawDeg = 0.0f; q.pitchDeg = 0.0f; q.distance = 400.0f; q.fovDeg = 45.0f;
            q.pivot[0] = q.pivot[1] = q.pivot[2] = 0.0f;
            q.panPixels(0.0f, 100.0f, 1000.0f);
            check(q.pivot[2] > 1.0f, "drag down pans the pivot UP (+Z), the engine's up axis");

            PreviewCamera near_ = q, far_ = q;
            near_.pivot[2] = far_.pivot[2] = 0.0f;
            near_.distance = 100.0f; far_.distance = 1000.0f;
            near_.panPixels(0.0f, 100.0f, 1000.0f);
            far_.panPixels(0.0f, 100.0f, 1000.0f);
            check(far_.pivot[2] > near_.pivot[2] * 9.0f,
                  "pan distance scales with the orbit distance");

            PreviewCamera z = q;
            const f32 before = z.pivot[2];
            z.panPixels(50.0f, 50.0f, 0.0f);
            check(z.pivot[2] == before, "a zero-height viewport pans nothing instead of dividing by it");
        }

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

    AVER_INFO("=== resize ===");
    {
        const u32 texBefore     = static_cast<u32>(dev.factory.textures.size());
        const u32 waitedBefore  = dev.factory.waited;
        const u32 destroyBefore = dev.factory.destroyedTextures;
        const u64 idBefore      = p->uiTextureId();

        check(p->resize(1600, 900), "a resize to a new size succeeds");
        check(p->width() == 1600 && p->height() == 900, "and the reported size follows");
        check(dev.factory.waited > waitedBefore, "the GPU is DRAINED first -- the UI may still be sampling");
        check(dev.factory.textures.size() == texBefore + 2, "a new colour and depth pair is created");
        check(dev.factory.destroyedTextures == destroyBefore + 2, "and the old pair is released");
        check(p->uiTextureId() != idBefore, "the UI texture id is RE-FETCHED, not carried over");

        const std::vector<rhi::TextureDesc>& tx = dev.factory.textures;
        check(tx[tx.size()-2].width == 1600 && tx[tx.size()-2].height == 900,
              "the new colour target is NON-SQUARE, at the asked size");

        MockContext ctx;
        p->setDrawList({});
        p->prePass(ctx);
        const std::vector<Call> vp2 = ctx.ofKind(Call::Kind::Viewport);
        check(vp2.size() == 1 && vp2[0].c == 1600 && vp2[0].d == 900,
              "and the pass's viewport follows the new target rather than the old one");

        const u32 waitedIdem = dev.factory.waited;
        check(p->resize(1600, 900), "resizing to the SAME size succeeds");
        check(dev.factory.waited == waitedIdem, "and does nothing at all -- no drain, no reallocation");

        check(!p->resize(0, 900), "a zero extent is refused rather than creating a degenerate target");
        check(p->width() == 1600 && p->height() == 900, "and the old target is still the live one");
    }

    AVER_INFO("=== caller-generated meshes ===");
    {
        // A REGRESSION, and a sharp one. generated() first took the vertices and indices directly,
        // which made a caller wanting "give me the handle if you have it, and only build the shape
        // if you don't" pass empty vectors as a probe -- indistinguishable from "I generated
        // nothing", which this cache records as a permanent miss. The probe poisoned its own key and
        // the real geometry that followed was never uploaded at all: the fluid component preview
        // drew nothing, silently, with the cache reporting a perfectly ordinary miss. Taking the
        // BUILDER instead removes the ambiguity, and these checks are about what reached the device.
        MockDevice md;
        PreviewMeshCache cache;
        int builds = 0;
        const auto buildBox = [&builds](std::vector<rhi::MeshVertex>& v, std::vector<u32>& i) {
            ++builds;
            v.push_back({ 0.0f, 0.0f, 0.0f, 0, 0, 1, 0, 0});
            v.push_back({30.0f, 0.0f, 0.0f, 0, 0, 1, 1, 0});
            v.push_back({30.0f, 40.0f, 0.0f, 0, 0, 1, 1, 1});   // 3-4-5: exactly 50 from the origin
            i.push_back(0); i.push_back(1); i.push_back(2);
        };

        f32 radius = -1.0f;
        const rhi::MeshHandle first = cache.generated(md, "$gen/box", buildBox, &radius);
        check(first != 0, "a generated mesh comes back with a live handle");
        check(builds == 1, "the builder ran once");
        check(md.meshUploads.size() == 1 && md.meshUploads[0].first == 3 &&
              md.meshUploads[0].second == 3,
              "and the vertices it produced are what reached createMesh -- not an empty probe");
        check(std::fabs(radius - 50.0f) < 0.01f,
              "the reported radius is the furthest vertex, 3-4-5 from the origin");

        radius = -1.0f;
        const rhi::MeshHandle again = cache.generated(md, "$gen/box", buildBox, &radius);
        check(again == first, "asking again for the same key returns the same handle");
        check(builds == 1, "WITHOUT building the shape a second time -- the point of the callback");
        check(md.meshUploads.size() == 1, "and without a second upload");
        check(std::fabs(radius - 50.0f) < 0.01f, "the cached radius is reported too");

        int emptyBuilds = 0;
        const auto buildNothing = [&emptyBuilds](std::vector<rhi::MeshVertex>&, std::vector<u32>&) {
            ++emptyBuilds;
        };
        check(cache.generated(md, "$gen/empty", buildNothing) == 0,
              "a builder that produces nothing yields no handle");
        check(cache.generated(md, "$gen/empty", buildNothing) == 0, "and still none on the next ask");
        check(emptyBuilds == 1,
              "which is cached as a miss, so a hopeless key is not rebuilt every frame");
        check(md.meshUploads.size() == 1, "and nothing empty was ever handed to the device");

        check(cache.generated(md, "$gen/other", buildBox) != first,
              "a different key is a different mesh");
    }

#if AVER_MODULE_PBR
    AVER_INFO("=== the material path ===");
    {
        const MockFactory& f = dev.factory;

        // NOTHING HAS REGISTERED A GRAPH YET, in this process or anywhere earlier in this file --
        // the two shaders and the one pipeline "what init creates" checked above are the whole
        // story, and neither shader's recorded text mentions the material system at all. This is
        // "a project with no graphs is completely unaffected", checked BEFORE the block below
        // registers one so it cannot be an accident of ordering.
        check(pbr::materialGraphs().count() == 0, "no material graph exists yet in this process");
        for (const rhi::ShaderDesc& sd : f.shaders) {
            const bool clean =
                (!sd.source || std::string(sd.source).find("AVER_MATERIAL_GRAPH") == std::string::npos) &&
                (!sd.prelude || std::string(sd.prelude).find("AVER_MATERIAL_GRAPH") == std::string::npos);
            check(clean, "no recorded shader's source or prelude mentions AVER_MATERIAL_GRAPH");
        }
        check(f.pipelines.size() == 1, "and still just the one pipeline");

        const usize shadersBefore = f.shaders.size();
        const usize pipelinesBefore = f.pipelines.size();

        pbr::materialGraphs().clear();
        const u32 gid = pbr::materialGraphs().add("$test/flat", "ActorPreviewTest material",
                                                   flatColorGraph());
        check(gid != 0, "the test graph compiles");

        std::vector<PreviewDraw> draws;
        PreviewDraw plain;
        plain.mesh = 1;
        draws.push_back(plain);              // materialGraphId 0: the default every caller gets
        PreviewDraw shaded;
        shaded.mesh = 2;
        shaded.materialGraphId = gid;
        draws.push_back(shaded);

        MockContext ctx;
        p->setDrawList(draws);
        p->prePass(ctx);

        check(f.pipelines.size() == pipelinesBefore + 1,
              "a graph registered before the frame gets its own, second, pipeline");
        check(f.shaders.size() == shadersBefore + 2,
              "and two more shaders: the material vertex and pixel");

        const rhi::ShaderDesc& matVs = f.shaders[shadersBefore];
        const rhi::ShaderDesc& matPs = f.shaders[shadersBefore + 1];
        check(matVs.stage == rhi::ShaderStage::Vertex, "the first new shader is the vertex stage");
        check(matPs.stage == rhi::ShaderStage::Pixel, "and the second is the pixel stage");

        const std::string matPrelude = matPs.prelude ? matPs.prelude : "";
        check(matPrelude.find("AVER_MATERIAL_GRAPH") != std::string::npos,
              "the material pixel shader's prelude carries the define");
        check(matPrelude.find("averEvalMaterial") != std::string::npos,
              "and the graph's own generated averEvalMaterial");

        const std::string matDefines = matPs.defines ? matPs.defines : "";
        check(matDefines.find("AVER_MATERIAL_SRV=") != std::string::npos,
              "and its ShaderDesc declares the material texture registers");

        const std::vector<Call> pipe = ctx.ofKind(Call::Kind::Pipeline);
        check(pipe.size() == 2, "two pipeline binds: the plain draw, then the graph-shaded one");
        if (pipe.size() == 2) {
            // Handle 1 is MockFactory's very first pipeline -- the simple one "what init creates"
            // built, and the only pipeline that has ever existed until this block.
            check(pipe[0].a == 1, "a draw with materialGraphId 0 still selects the simple pipeline");
            check(pipe[1].a != pipe[0].a, "and the graph-shaded draw selects the new one");
        }
    }
#endif

    AVER_INFO("=== teardown ===");
    {
        const u32 waitedBefore = dev.factory.waited;
        delete p;
        check(dev.factory.waited > waitedBefore, "the GPU is drained before the targets go");
        check(dev.factory.destroyedTextures >= 2, "and both targets are released");
    }

    if (g_failures == 0) AVER_INFO("=== all actor preview tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
