// SubViewportHostTest -- the host half of sub-viewports (G4 R5): a camera entity's view-projection, the secondary world walk (DrawWorldPass::View), the sun and
// fill taken from the sky, the project's cost guard, and SubViewportHost end to end against a recording device -- views planned from the framework registry,
// a feature per view with the right camera and draw list, the texture published to scripts and to materials, resizes, the on-screen list, and nothing at all
// when no view exists. A real scene::World and GameContent; no window, no GPU.
#include "aver/core/Log.hpp"
#include "aver/framework/framework_subview_abi.h"
#include "aver/game/GameContent.hpp"
#include "aver/game/GameRender.hpp"
#include "aver/game/SubViewportCamera.hpp"
#include "aver/game/SubViewportHost.hpp"
#include "aver/pbr/Material.hpp"
#include "aver/pbr/MaterialSystem.hpp"
#include "aver/render/preview/ActorPreview.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/RenderMasks.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

namespace {

// ---- the recording device (the shape ActorPreviewTest uses) ----

struct MockFactory final : public rhi::IResourceFactory {
    std::vector<rhi::TextureDesc> textures;
    std::vector<rhi::ShaderDesc> shaders;
    std::vector<rhi::GraphicsPipelineDesc> pipelines;
    std::vector<rhi::TextureHandle> destroyed;
    u32 waited = 0;

    rhi::TextureHandle createTexture(const rhi::TextureDesc& d) override { textures.push_back(d); return static_cast<rhi::TextureHandle>(textures.size()); }
    rhi::BufferHandle createBuffer(const rhi::BufferDesc&) override { return 1; }
    rhi::ShaderHandle createShader(const rhi::ShaderDesc& d) override { shaders.push_back(d); return static_cast<rhi::ShaderHandle>(shaders.size()); }
    rhi::PipelineHandle createGraphicsPipeline(const rhi::GraphicsPipelineDesc& d) override { pipelines.push_back(d); return static_cast<rhi::PipelineHandle>(pipelines.size()); }
    rhi::PipelineHandle createComputePipeline(const rhi::ComputePipelineDesc&) override { return 0; }
    rhi::BindlessTableHandle createBindlessTextureTable(u32) override { return 0; }
    void destroyBindlessTextureTable(rhi::BindlessTableHandle) override {}
    bool setBindlessTexture(rhi::BindlessTableHandle, u32, rhi::TextureHandle) override { return false; }
    u32  bindlessTableCapacity(rhi::BindlessTableHandle) const override { return 0; }
    rhi::BindingSetHandle createBindingSet(const rhi::BindingSetDesc&) override { return 1; }
    rhi::BlasHandle createBlas(rhi::MeshHandle) override { return 0; }
    rhi::TlasHandle createTlas(u32) override { return 0; }
    void destroyTexture(rhi::TextureHandle h) override { destroyed.push_back(h); }
    void destroyBuffer(rhi::BufferHandle) override {}
    void destroyShader(rhi::ShaderHandle) override {}
    void destroyPipeline(rhi::PipelineHandle) override {}
    void destroyBindingSet(rhi::BindingSetHandle) override {}
    void setSrv(rhi::BindingSetHandle, u32, rhi::TextureHandle, u32) override {}
    void setUav(rhi::BindingSetHandle, u32, rhi::TextureHandle, u32) override {}
    void clearSrv(rhi::BindingSetHandle, u32) override {}
    void setSrvTlas(rhi::BindingSetHandle, u32, rhi::TlasHandle) override {}
    void setSrvBuffer(rhi::BindingSetHandle, u32, rhi::BufferHandle, u32, u32, u32) override {}
    void setUavBuffer(rhi::BindingSetHandle, u32, rhi::BufferHandle, u32, u32, u32) override {}
    bool writeBuffer(rhi::BufferHandle, const void*, u64, u64) override { return true; }
    bool readBuffer(rhi::BufferHandle, void*, u64, u64) override { return true; }
    bool textureInfo(rhi::TextureHandle, rhi::TextureDesc&) const override { return false; }
    void waitIdle() override { ++waited; }
};

struct Call {
    enum class Kind { Pipeline, Targets, ClearDepth, ClearColor, Constants, ConstantBuffer, DrawMesh, Other };
    Kind kind = Kind::Other;
    u32 a = 0, b = 0;
    std::vector<f32> payload;
};

struct MockContext final : public rhi::IRenderContext {
    std::vector<Call> calls;
    void setPipeline(rhi::PipelineHandle p) override { calls.push_back({Call::Kind::Pipeline, p}); }
    void setViewport(u32, u32, u32, u32) override {}
    void setScissor(u32, u32, u32, u32) override {}
    void setRenderTargets(const rhi::TextureHandle* c, u32 n, rhi::TextureHandle) override { calls.push_back({Call::Kind::Targets, n ? c[0] : 0u, n}); }
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
    void drawMeshInstanced(rhi::MeshHandle, const f32*, u32) override {}
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
    void textureBarrier(rhi::TextureHandle, rhi::ResourceState, rhi::ResourceState, u32) override {}
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
    std::vector<rhi::IRenderFeature*> features;
    u32 drawMeshCalls = 0, bindingCalls = 0;
    rhi::DeviceCaps caps() const override { rhi::DeviceCaps c; c.shaderModel = 66; c.dxcAvailable = true; return c; }
    rhi::Backend backend() const override { return rhi::Backend::Null; }
    const char* adapterName() const override { return "recording device"; }
    rhi::IResourceFactory* resources() override { return &factory; }
    rhi::ISwapchain* createSwapchain(const rhi::SwapchainDesc&) override { return nullptr; }
    void beginFrame() override {}
    void endFrame() override {}
    void addRenderFeature(rhi::IRenderFeature* f) override { features.push_back(f); }
    void removeRenderFeature(rhi::IRenderFeature* f) override {
        for (usize i = 0; i < features.size(); ++i) if (features[i] == f) { features.erase(features.begin() + static_cast<std::ptrdiff_t>(i)); return; }
    }
    void drawMesh(rhi::MeshHandle, const f32[16], const f32[4], f32, f32) override { ++drawMeshCalls; }
    void setDrawBinding(rhi::BindingSetHandle, const void*, u32) override { ++bindingCalls; }
};

// ---- a small world ----

struct Scene {
    scene::World& w = scene::World::instance();
    game::GameContent content;
    std::vector<scene::Entity> all;
    scene::Entity camera = scene::kInvalidEntity;

    scene::Entity place(const char* name, Vec3 p, u64 meshId, rhi::MeshHandle handle) {
        const scene::Entity e = w.create(name);
        Transform xf;
        xf.position = p;
        w.setLocalTransform(e, xf);
        auto* mr = static_cast<scene::CMeshRenderer*>(w.addComponent(e, scene::kComponentMeshRenderer));
        *mr = scene::CMeshRenderer{};
        mr->mesh = meshId;
        mr->aabbMin[0] = mr->aabbMin[1] = mr->aabbMin[2] = -50.0f;
        mr->aabbMax[0] = mr->aabbMax[1] = mr->aabbMax[2] = 50.0f;
        mr->flags = scene::kMeshRendererVisible;
        content.registerMesh(meshId, handle, {Vec3{-50, -50, -50}, Vec3{50, 50, 50}});
        all.push_back(e);
        return e;
    }
    scene::Entity makeCamera(Vec3 p, f32 yawRad = 0.0f) {
        camera = w.create("view camera");
        Transform xf;
        xf.position = p;
        xf.rotation = Quat::fromAxisAngle(Vec3{0, 0, 1}, yawRad);
        w.setLocalTransform(camera, xf);
        auto* cc = static_cast<scene::CCamera*>(w.addComponent(camera, scene::kComponentCamera));
        *cc = scene::CCamera{};
        cc->fovYRad = radians(60.0f);
        w.flush();
        all.push_back(camera);
        return camera;
    }
    ~Scene() {
        for (scene::Entity e : all) w.destroy(e);
        w.flush();
    }
};

struct Seen {
    std::vector<rhi::MeshHandle> meshes;
    std::vector<bool> translucent;
};

void sink(rhi::MeshHandle mesh, const f32[16], const f32[4], f32, f32, rhi::BindingSetHandle, const void*, u32, bool translucent, void* user) {
    auto* s = static_cast<Seen*>(user);
    s->meshes.push_back(mesh);
    s->translucent.push_back(translucent);
}

bool has(const Seen& s, rhi::MeshHandle m) {
    for (rhi::MeshHandle x : s.meshes) if (x == m) return true;
    return false;
}

} // namespace

static void testCamera() {
    AVER_INFO("=== a camera entity's view ===");
    Scene s;
    const scene::Entity cam = s.makeCamera(Vec3{100, 200, 300}, radians(90.0f));   // looks along +Y
    game::ViewCamera v;
    check(game::viewCameraOf(s.w, cam, 16.0f / 9.0f, v), "a live camera entity gives a view");
    check(near(v.eye.x, 100, 1e-3f) && near(v.eye.y, 200, 1e-3f) && near(v.eye.z, 300, 1e-3f), "the eye is the entity's position");
    check(near(v.forward.x, 0, 1e-4f) && near(v.forward.y, 1, 1e-4f) && near(v.up.z, 1, 1e-4f), "it looks along the entity's +X (here turned to +Y), with +Z up");
    f32 x = 9, y = 9, z = 9;
    check(projectToNdc(v.viewProj, Vec3{100, 700, 300}, x, y, z) && near(x, 0, 1e-4f) && near(y, 0, 1e-4f) && z > 0 && z < 1, "a point straight ahead projects to the centre of the picture");
    check(projectToNdc(v.viewProj, Vec3{100, 700, 300 + 500.0f * std::tan(radians(30.0f))}, x, y, z) && near(y, 1.0f, 1e-3f), "and one at the half field of view lands on the top edge");
    check(!projectToNdc(v.viewProj, Vec3{100, -700, 300}, x, y, z), "one behind the camera does not project");
    check(!game::viewCameraOf(s.w, scene::kInvalidEntity, 1.0f, v) && !game::viewCameraOf(s.w, static_cast<scene::Entity>(0x7FFFFFF0u), 1.0f, v), "no entity, no view");

    // A zero-filled CCamera (a component added with no defaults) reads as the play camera's lens.
    const scene::Entity bare = s.w.create("bare camera");
    s.all.push_back(bare);
    s.w.addComponent(bare, scene::kComponentCamera);
    s.w.flush();
    game::ViewCamera b;
    check(game::viewCameraOf(s.w, bare, 1.0f, b) && near(b.lens.fovRad, radians(60.0f), 1e-5f) && b.lens.nearCm == 2.0f && b.lens.farCm == 200000.0f, "a zero-filled CCamera reads as 60 degrees, 2 cm to 2 km");

    // Lens fields: orthographic, cull mask and the h/v offsets.
    auto* cc = s.w.component<scene::CCamera>(cam, scene::kComponentCamera);
    cc->projection = 1;
    cc->sizeCm = 400.0f;
    cc->cullInv = scene::storeMask(0x6u);
    game::ViewCamera o;
    check(game::viewCameraOf(s.w, cam, 2.0f, o) && o.lens.kind == ProjectionKind::Orthographic && isOrthographicViewProj(&o.viewProj.m[0][0]),
          "an orthographic CCamera gives an orthographic view-projection");
    check(o.cullInv == scene::storeMask(0x6u), "and the camera's cull mask");
    // This engine is left-handed with +X forward and +Z up, so a camera turned to look along +Y has its right toward -X.
    check(projectToNdc(o.viewProj, Vec3{100 - 400, 700, 300}, x, y, z) && near(x, 1.0f, 1e-3f), "size 400 at aspect 2 and keep-height: the view is 800 cm wide, so 400 to its right is the edge");
    cc->projection = 0;
    cc->hOffsetCm = 100.0f;
    game::ViewCamera h;
    check(game::viewCameraOf(s.w, cam, 1.0f, h) && near(h.eye.x, 0.0f, 1e-3f) && near(h.eye.y, 200, 1e-3f), "h_offset slides the eye along the camera's right (here -X for a camera looking +Y)");

    // makeViewCamera copes with a view straight up.
    const game::ViewCamera up = game::makeViewCamera(Vec3{0, 0, 0}, Vec3{0, 0, 1}, Vec3{0, 0, 1}, CameraProjection{}, 1.0f);
    check(std::fabs(dot(up.up, up.forward)) < 1e-4f && up.up.sizeSquared() > 0.9f, "looking straight up still has a usable up vector");
}

static void testLighting() {
    AVER_INFO("=== the sun and fill from the sky ===");
    rhi::SkyAtmosphere sky;
    const game::SceneViewLighting l = game::sceneViewLighting(sky);
    check(near(std::sqrt(l.sunDir[0] * l.sunDir[0] + l.sunDir[1] * l.sunDir[1] + l.sunDir[2] * l.sunDir[2]), 1.0f, 1e-5f), "the sun direction is a unit vector");
    check(near(l.sunRadiance[0], 1.6f, 1e-5f), "the engine's 3.0 sun is the studio's 1.6 key");
    sky.sunIntensity = 1.5f;
    sky.sunColor[0] = 1.0f; sky.sunColor[1] = 0.5f; sky.sunColor[2] = 0.0f;
    const game::SceneViewLighting dim = game::sceneViewLighting(sky);
    check(near(dim.sunRadiance[0], 0.8f, 1e-5f) && near(dim.sunRadiance[1], 0.4f, 1e-5f) && dim.sunRadiance[2] == 0.0f, "half the intensity halves the radiance, and the colour tints it");
    sky.sunIntensity = 1000.0f;
    check(near(game::sceneViewLighting(sky).sunRadiance[0], 6.4f, 1e-4f), "a blown-out sun is clamped to four times the key");
    sky.sunIntensity = -2.0f;
    check(game::sceneViewLighting(sky).sunRadiance[0] == 0.0f, "a negative one lights nothing");
    sky.sunDirection[0] = sky.sunDirection[1] = sky.sunDirection[2] = 0.0f;
    const game::SceneViewLighting zero = game::sceneViewLighting(sky);
    check(near(zero.sunDir[2], 0.86f, 1e-5f), "a zero direction keeps the default instead of NaN");
    rhi::SkyAtmosphere base;
    const game::SceneViewLighting amb = game::sceneViewLighting(base);
    base.skyLightIntensity = 2.0f;
    const game::SceneViewLighting amb2 = game::sceneViewLighting(base);
    check(amb.ambient[2] > amb.ambient[0] && near(amb2.ambient[1], amb.ambient[1] * 2.0f, 1e-5f), "the fill is blue-ish for the default dome and follows the sky-light multiplier");

    const game::SubViewportSettings d = game::subViewportSettingsOf(-1, -1, -1.0f);
    check(d.maxViews == 4 && d.rendersPerFrame == 2 && d.maxPixelsMega == 8.0f, "unstated keys are the defaults");
    const game::SubViewportSettings k = game::subViewportSettingsOf(99, 99, 1000.0f);
    check(k.maxViews == 16 && k.rendersPerFrame == 16 && k.maxPixelsMega == 256.0f, "absurd keys are clamped");
    const game::SubViewportSettings off = game::subViewportSettingsOf(0, 0, 0.0f);
    check(off.maxViews == 0 && off.rendersPerFrame == 2 && off.maxPixelsMega == 8.0f, "0 views turns it off; 0 renders and 0 pixels are not values");
}

static void testViewWalk() {
    AVER_INFO("=== DrawWorldPass::View: a secondary camera's walk ===");
    Scene s;
    MockDevice dev;
    const scene::Entity cam = s.makeCamera(Vec3{0, 0, 100});
    const scene::Entity front = s.place("front", Vec3{600, 0, 100}, 101, 11);
    const scene::Entity behind = s.place("behind", Vec3{-600, 0, 100}, 102, 12);
    const scene::Entity side = s.place("side", Vec3{600, 9000, 100}, 103, 13);
    const scene::Entity masked = s.place("masked", Vec3{800, 0, 100}, 104, 14);
    const scene::Entity ranged = s.place("ranged", Vec3{700, 100, 100}, 105, 15);
    const scene::Entity hidden = s.place("hidden", Vec3{500, -100, 100}, 106, 16);
    (void)front; (void)behind; (void)side;
    s.w.component<scene::CMeshRenderer>(masked, scene::kComponentMeshRenderer)->layersInv = scene::storeMask(0x2u);
    scene::CMeshRenderer* rr = s.w.component<scene::CMeshRenderer>(ranged, scene::kComponentMeshRenderer);
    rr->visEndCm = 300.0f;   // the camera is 700 away
    rr->rangeState = 0;
    s.w.component<scene::CMeshRenderer>(hidden, scene::kComponentMeshRenderer)->flags = 0;   // not visible
    s.w.flush();

    game::ViewCamera vc;
    check(game::viewCameraOf(s.w, cam, 1.0f, vc), "the view camera");

    Seen seen;
    game::DrawWorldOptions opt;
    opt.pass = game::DrawWorldPass::View;
    opt.onViewDraw = &sink;
    opt.user = &seen;
    opt.cameraCullInv = scene::storeMask(0x1u);   // the camera draws layer 1 only
    opt.viewerPos = vc.eye;
    opt.haveViewerPos = true;
    opt.suppressLog = true;
    game::SceneDrawStats stats;
    game::drawWorld(dev, vc.viewProj, s.content, stats, nullptr, nullptr, opt);

    check(has(seen, 11), "an entity in front is delivered");
    check(!has(seen, 12), "one behind the camera is not");
    check(!has(seen, 13), "one far off to the side is outside the frustum");
    check(!has(seen, 14), "one on a layer the camera's cull mask excludes is not delivered (it is hidden from this camera)");
    check(!has(seen, 15), "one past its visibility range is skipped");
    check(!has(seen, 16), "one that is not visible is skipped");
    check(seen.meshes.size() == 1, "and nothing else is");
    check(dev.drawMeshCalls == 0 && dev.bindingCalls == 0, "the walk made no draw and no binding on the device");
    check(stats.drawn == 0 && stats.culled == 0 && stats.ownerHidden == 0 && stats.lastDrawn == -1, "and touched none of the primary view's counters");
    check(s.w.component<scene::CMeshRenderer>(ranged, scene::kComponentMeshRenderer)->rangeState == 0,
          "the visibility-range hysteresis memory is left as the primary walk wrote it");

    // The same entity through the primary (Colour) walk draws on the device and writes the range memory: the contrast that proves the View pass differs.
    game::DrawWorldOptions colour;
    colour.suppressLog = true;
    colour.viewerPos = vc.eye;
    colour.haveViewerPos = true;
    game::SceneDrawStats cstats;
    game::drawWorld(dev, vc.viewProj, s.content, cstats, nullptr, nullptr, colour);
    check(dev.drawMeshCalls > 0 && cstats.drawn > 0, "the primary walk over the same world does draw");
    check(s.w.component<scene::CMeshRenderer>(ranged, scene::kComponentMeshRenderer)->rangeState != 0, "and records the range verdict that the view pass left alone");

    // With no sink the pass delivers nothing and still touches nothing.
    game::DrawWorldOptions nosink;
    nosink.pass = game::DrawWorldPass::View;
    nosink.suppressLog = true;
    const u32 before = dev.drawMeshCalls;
    game::SceneDrawStats nstats;
    game::drawWorld(dev, vc.viewProj, s.content, nstats, nullptr, nullptr, nosink);
    check(dev.drawMeshCalls == before && nstats.lastDrawn == -1, "a view walk without a sink does nothing");
}

static void testHost() {
    AVER_INFO("=== SubViewportHost end to end ===");
    aver_fw_subview_reset();
    aver_fw_subview_configure(4, 2, 8.0f);

    Scene s;
    MockDevice dev;
    const scene::Entity camE = s.makeCamera(Vec3{0, 0, 100});
    s.place("front", Vec3{600, 0, 100}, 201, 21);
    s.place("front2", Vec3{900, 50, 100}, 202, 22);
    s.place("behind", Vec3{-600, 0, 100}, 203, 23);
    s.w.flush();

    std::vector<rhi::TextureHandle> retired;
    game::SubViewportHost host;
    host.attach(dev, [](rhi::TextureHandle t, void* user) { static_cast<std::vector<rhi::TextureHandle>*>(user)->push_back(t); }, &retired);

    rhi::SkyAtmosphere sky;
    game::DrawWorldOptions walk;
    game::SubViewportHost::Inputs in;
    in.content = &s.content;
    in.walk = &walk;
    in.sky = &sky;
    pbr::MaterialSystem ms;   // not initialised: the host only reads it for average colours and names a runtime texture on it
    in.materials = &ms;

    // The cost guard: no view in the registry, nothing is created.
    in.frame = 1;
    host.update(in);
    check(dev.factory.textures.empty() && dev.features.empty() && host.liveViews() == 0 && host.lastFrameRendered() == 0,
          "with no view in the registry the host creates no texture and no feature");

    // A material whose base colour is the view's texture.
    pbr::MaterialDesc md;
    md.name = "M_Screen";
    md.textures[static_cast<u32>(pbr::TextureSlot::BaseColor)].path = "@rt:viewport.Main/Screen";
    pbr::MaterialLibrary& lib = pbr::MaterialLibrary::get();
    const pbr::MaterialHandle mat = lib.create(md);
    lib.consumeDirty(mat);   // creation owes an upload; settle it so the touch below is the only thing that can make it dirty again

    // A view: 256x128, the camera entity, updated while a material shows it.
    const int32_t id = aver_fw_subview_create("Main/Screen", 256, 128);
    check(id > 0, "a view is created through the ABI");
    aver_fw_subview_set_update(id, AVER_FW_SUBVIEW_UPDATE_WHEN_VISIBLE);
    aver_fw_subview_set_camera(id, static_cast<int32_t>(camE));
    aver_fw_subview_set_clear_color(id, 0.3f, 0.3f, 0.3f, 1.0f);

    in.frame = 2;
    host.update(in);
    check(host.lastFrameRendered() == 0 && dev.factory.textures.empty(), "a WhenVisible view nothing shows is not drawn and has no target");

    aver_fw_subview_consumer(id, AVER_FW_SUBVIEW_CONSUMER_MATERIAL, +1);
    in.frame = 3;
    host.update(in);
    check(host.lastFrameRendered() == 1 && host.liveViews() == 1, "with a material showing it the view is drawn");
    check(dev.factory.textures.size() == 2 && dev.factory.textures[0].width == 256 && dev.factory.textures[0].height == 128, "its target is 256x128 (colour and depth)");
    check(dev.features.size() == 1, "its feature is registered with the device");
    check(dev.factory.pipelines.size() == 1 && dev.factory.shaders.size() == 2, "and builds the mesh pipeline only, none of the studio's chrome");
    const rhi::TextureHandle colour = host.texture(id);
    check(colour != 0 && aver_fw_subview_texture(id) == colour, "the texture is published to scripts through the ABI");
    check(ms.runtimeTexture("viewport.Main/Screen") == colour, "and to materials as the runtime texture '@rt:viewport.Main/Screen'");
    check(lib.consumeDirty(mat), "the material that names it was touched, so its binding set is rewritten");
    int32_t info[8] = {};
    aver_fw_subview_info(id, info);
    check(info[6] == 1 && info[7] == 1, "the registry saw one render and one texture generation");

    // The feature draws the view's picture: its camera, its clear colour, and only what the camera sees.
    {
        MockContext ctx;
        dev.features[0]->prePass(ctx);
        check(ctx.count(Call::Kind::DrawMesh) == 2, "the feature draws the two meshes in front of the camera and not the one behind it");
        const std::vector<Call> targets = ctx.ofKind(Call::Kind::Targets);
        check(targets.size() == 1 && targets[0].a == colour, "into the view's own colour target");
        check(ctx.count(Call::Kind::ClearColor) == 1, "cleared first (the first picture)");
        const std::vector<Call> cb = ctx.ofKind(Call::Kind::ConstantBuffer);
        game::ViewCamera vc;
        game::viewCameraOf(s.w, camE, 2.0f, vc);
        check(cb.size() == 1 && cb[0].payload.size() >= 20 && near(cb[0].payload[0], vc.viewProj.m[0][0], 1e-5f) && near(cb[0].payload[5], vc.viewProj.m[1][1], 1e-5f),
              "with the camera's view-projection at the aspect of the view (2:1)");
        check(cb.size() == 1 && near(cb[0].payload[18], 100.0f, 1e-4f), "and its eye");
        check(cb.size() == 1 && cb[0].payload[29] == 1.0f, "a view only a material reads stores linear values");
    }

    // A frame in which the registry plans nothing leaves the feature switched off.
    aver_fw_subview_consumer(id, AVER_FW_SUBVIEW_CONSUMER_MATERIAL, -1);
    in.frame = 4;
    host.update(in);
    {
        MockContext ctx;
        dev.features[0]->prePass(ctx);
        check(host.lastFrameRendered() == 0 && ctx.calls.empty(), "a view nothing shows any more is not drawn: its feature records nothing and keeps the last picture");
    }

    // A bigger target: rebuilt, the old texture is retired first, the generation moves, the material is told again.
    aver_fw_subview_consumer(id, AVER_FW_SUBVIEW_CONSUMER_UI, +1);
    aver_fw_subview_set_size(id, 512, 256);
    lib.consumeDirty(mat);
    in.frame = 5;
    host.update(in);
    check(retired.size() == 1 && retired[0] == colour, "a resize tells the UI to forget the old texture first");
    const rhi::TextureHandle colour2 = host.texture(id);
    check(colour2 != 0 && colour2 != colour && aver_fw_subview_texture(id) == colour2 && ms.runtimeTexture("viewport.Main/Screen") == colour2, "the new texture is published everywhere");
    aver_fw_subview_info(id, info);
    check(info[7] == 2 && info[4] == 512 && info[5] == 256, "with generation 2");
    check(lib.consumeDirty(mat), "and the material is touched again");
    {
        MockContext ctx;
        dev.features[0]->prePass(ctx);
        const std::vector<Call> cb = ctx.ofKind(Call::Kind::ConstantBuffer);
        check(cb.size() == 1 && cb[0].payload[29] == 0.0f, "a view the UI also shows stores display values");
    }

    // On screen: a rectangle in the UI's space, listed once it has a picture.
    host.update(in);
    game::SubViewportHost::OnScreenView osv[4];
    check(host.onScreen(osv, 4) == 0, "a view that is not on screen is not listed");
    aver_fw_subview_set_screen_rect(id, 10, 20, 640, 360, 1);
    check(host.onScreen(osv, 4) == 1 && osv[0].texture == colour2 && osv[0].x == 10 && osv[0].w == 640, "an on-screen view is listed with its texture and rectangle");
    check(host.onScreen(osv, 0) == 0, "no room, none listed");

    // Two views over the render budget: only two are drawn a frame.
    int32_t b = aver_fw_subview_create("Main/B", 64, 64), c = aver_fw_subview_create("Main/C", 64, 64);
    for (int32_t v : {b, c}) {
        aver_fw_subview_set_camera(v, static_cast<int32_t>(camE));
        aver_fw_subview_set_update(v, AVER_FW_SUBVIEW_UPDATE_ALWAYS);
    }
    in.frame = 6;
    host.update(in);
    check(host.lastFrameRendered() == 2 && host.liveViews() == 3, "three views due, two drawn: the per-frame budget");
    in.frame = 7;
    host.update(in);
    check(host.lastFrameRendered() == 2, "and the other gets its turn next frame");

    // A camera that is gone: the view keeps its last picture and says so; nothing crashes.
    aver_fw_subview_set_camera(c, 0x7FFFFFF0);
    in.frame = 8;
    host.update(in);
    check(host.liveViews() == 3, "a view whose camera entity has gone stays alive");

    // Destroying views: the host retires their features and targets.
    const usize featuresBefore = dev.features.size();
    aver_fw_subview_destroy(b);
    aver_fw_subview_destroy(c);
    in.frame = 9;
    host.update(in);
    check(dev.features.size() == featuresBefore - 2 && host.liveViews() == 1, "views the registry dropped lose their features");
    check(aver_fw_subview_texture(b) == 0, "and their texture entries are gone from the ABI");

    aver_fw_subview_destroy(id);
    in.frame = 10;
    host.update(in);
    check(host.liveViews() == 0 && dev.features.empty() && ms.runtimeTexture("viewport.Main/Screen") == 0, "the last view's destruction clears the runtime texture the materials named");
    check(lib.consumeDirty(mat), "and touches the material so it goes back to its fallback");
    check(dev.factory.waited > 0 && dev.factory.destroyed.size() >= 2, "the GPU was drained and the targets freed");

    lib.destroy(mat);
    host.detach();
    check(host.liveViews() == 0, "detach leaves nothing");
    aver_fw_subview_reset();
}

int main() {
    AVER_INFO("SubViewportHostTest");
    testCamera();
    testLighting();
    testViewWalk();
    testHost();
    if (g_failures) {
        AVER_ERROR("SubViewportHostTest: {} failure(s)", g_failures);
        return 1;
    }
    AVER_INFO("SubViewportHostTest: ok");
    return 0;
}
