// FirstPersonWalkTest -- the scene walk's half of first-person rendering (docs/plans/FIRST_PERSON.md, step 3 and the Voxi side of step 5): a mesh flagged
// kRenderFirstPerson, or under DrawWorldOptions::firstPersonRoot, reaches the device with the first-person bit, is drawn although the main frustum says it is out,
// and reaches Voxi as a mover with no shadow and no ray unless kRenderFirstPersonCasts, which puts it in the owner-hidden lane. A recording device that forwards
// each draw to a real VoxiRenderer stands in for the backend (the way the backend broadcasts submitDraw), so the verdicts read are Voxi's own. No window, no GPU.
#include "aver/core/Log.hpp"
#include "aver/game/GameContent.hpp"
#include "aver/game/GameRender.hpp"
#include "aver/game/SubViewportCamera.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"
#include "aver/voxi/VoxiRenderer.hpp"
#if AVER_TEST_HAS_PT
#include "aver/pt/PtSceneView.hpp"
#endif

#include <memory>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

namespace {

struct MockFactory final : public rhi::IResourceFactory {
    rhi::TextureHandle createTexture(const rhi::TextureDesc&) override { return 1; }
    rhi::BufferHandle createBuffer(const rhi::BufferDesc&) override { return 1; }
    rhi::ShaderHandle createShader(const rhi::ShaderDesc&) override { return 1; }
    rhi::PipelineHandle createGraphicsPipeline(const rhi::GraphicsPipelineDesc&) override { return 1; }
    rhi::PipelineHandle createComputePipeline(const rhi::ComputePipelineDesc&) override { return 0; }
    rhi::BindlessTableHandle createBindlessTextureTable(u32) override { return 0; }
    void destroyBindlessTextureTable(rhi::BindlessTableHandle) override {}
    bool setBindlessTexture(rhi::BindlessTableHandle, u32, rhi::TextureHandle) override { return false; }
    u32  bindlessTableCapacity(rhi::BindlessTableHandle) const override { return 0; }
    rhi::BindingSetHandle createBindingSet(const rhi::BindingSetDesc&) override { return 1; }
    rhi::BlasHandle createBlas(rhi::MeshHandle) override { return 0; }
    rhi::TlasHandle createTlas(u32) override { return 0; }
    void destroyTexture(rhi::TextureHandle) override {}
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
    void waitIdle() override {}
};

// Records the sticky first-person bit at each draw, and forwards the draw to Voxi as the backends broadcast it.
struct MockDevice final : public rhi::IDevice {
    struct Drawn { rhi::MeshHandle mesh; bool firstPerson; };
    MockFactory factory;
    voxi::VoxiRenderer* voxi = nullptr;
    bool fp = false;
    std::vector<Drawn> drawn;
    std::vector<rhi::MeshHandle> prepassed;
    rhi::DeviceCaps caps() const override { rhi::DeviceCaps c; c.shaderModel = 66; c.dxcAvailable = true; return c; }
    rhi::Backend backend() const override { return rhi::Backend::Null; }
    const char* adapterName() const override { return "recording device"; }
    rhi::IResourceFactory* resources() override { return &factory; }
    rhi::ISwapchain* createSwapchain(const rhi::SwapchainDesc&) override { return nullptr; }
    void beginFrame() override {}
    void endFrame() override {}
    void addRenderFeature(rhi::IRenderFeature*) override {}
    void removeRenderFeature(rhi::IRenderFeature*) override {}
    void setDrawFirstPerson(bool on) override { fp = on; }
    void drawMesh(rhi::MeshHandle m, const f32 world[16], const f32 color[4], f32 metallic, f32 roughness) override {
        drawn.push_back({m, fp});
        if (voxi) voxi->submitDraw(m, world, color, metallic, roughness, 0, nullptr, 0, false);
    }
    void drawMeshDepthPrepass(rhi::MeshHandle m, const f32[16], const f32[4]) override { prepassed.push_back(m); }
    void setDrawBinding(rhi::BindingSetHandle, const void*, u32) override {}

    const Drawn* find(rhi::MeshHandle m) const {
        for (const Drawn& d : drawn) if (d.mesh == m) return &d;
        return nullptr;
    }
};

struct Scene {
    scene::World& w = scene::World::instance();
    game::GameContent content;
    std::vector<scene::Entity> all;

    scene::Entity place(const char* name, Vec3 p, u64 meshId, rhi::MeshHandle handle, u32 renderFlags = 0, scene::Entity parent = scene::kInvalidEntity) {
        const scene::Entity e = w.create(name);
        Transform xf;
        xf.position = p;
        w.setLocalTransform(e, xf);
        if (parent != scene::kInvalidEntity) w.setParent(e, parent);
        if (meshId) {
            auto* mr = static_cast<scene::CMeshRenderer*>(w.addComponent(e, scene::kComponentMeshRenderer));
            *mr = scene::CMeshRenderer{};
            mr->mesh = meshId;
            mr->aabbMin[0] = mr->aabbMin[1] = mr->aabbMin[2] = -50.0f;
            mr->aabbMax[0] = mr->aabbMax[1] = mr->aabbMax[2] = 50.0f;
            mr->flags = scene::kMeshRendererVisible;
            mr->renderFlags = renderFlags;
            content.registerMesh(meshId, handle, {Vec3{-50, -50, -50}, Vec3{50, 50, 50}});
        }
        all.push_back(e);
        return e;
    }
    ~Scene() {
        for (scene::Entity e : all) if (w.valid(e)) w.destroy(e);
        w.flush();
    }
};

game::ViewCamera makeCamera(Scene& s) {
    const scene::Entity cam = s.w.create("camera");
    s.all.push_back(cam);
    scene::CCamera* cc = static_cast<scene::CCamera*>(s.w.addComponent(cam, scene::kComponentCamera));
    *cc = scene::CCamera{};
    cc->fovYRad = radians(60.0f);
    Transform xf;
    xf.position = Vec3{0, 0, 100};
    s.w.setLocalTransform(cam, xf);
    s.w.flush();
    game::ViewCamera vc;
    game::viewCameraOf(s.w, cam, 1.0f, vc);
    return vc;
}

constexpr rhi::MeshHandle kPlain = 11, kBehind = 12, kFp = 13, kFpBehind = 14, kFpCasts = 15, kChild = 16, kPlainAfter = 17;

} // namespace

static void testWalk() {
    AVER_INFO("=== the colour walk ===");
    Scene s;
    const game::ViewCamera vc = makeCamera(s);
    const scene::Entity root = s.place("fp root", Vec3{600, 0, 100}, 0, 0);
    s.place("fp", Vec3{500, 0, 100}, 101, kFp, scene::kRenderFirstPerson);
    s.place("plain after", Vec3{650, 0, 100}, 107, kPlainAfter);                                                  // drawn right after a flagged one: the bit must be gone
    s.place("fp behind the camera", Vec3{-900, 0, 100}, 102, kFpBehind, scene::kRenderFirstPerson);              // outside the main frustum
    s.place("fp casts", Vec3{550, 40, 100}, 103, kFpCasts, scene::kRenderFirstPerson | scene::kRenderFirstPersonCasts);
    s.place("plain", Vec3{600, -40, 100}, 104, kPlain);
    s.place("plain behind", Vec3{-900, 40, 100}, 105, kBehind);
    s.place("child of the root", Vec3{0, 0, 0}, 106, kChild, 0, root);
    s.w.flush();

    MockDevice dev;
    auto voxiPtr = std::make_unique<voxi::VoxiRenderer>();
    dev.voxi = voxiPtr.get();
    game::DrawWorldOptions opt;
    opt.suppressLog = true;
    opt.viewerPos = vc.eye;
    opt.haveViewerPos = true;
    opt.voxiRenderer = voxiPtr.get();
    opt.firstPersonRoot = root;
    game::SceneDrawStats stats;
    game::drawWorld(dev, vc.viewProj, s.content, stats, nullptr, nullptr, opt);

    const auto* fp = dev.find(kFp);
    const auto* fpBehind = dev.find(kFpBehind);
    const auto* fpCasts = dev.find(kFpCasts);
    const auto* child = dev.find(kChild);
    const auto* plain = dev.find(kPlain);
    check(fp && fp->firstPerson, "a flagged mesh is drawn with the first-person bit");
    check(fpBehind && fpBehind->firstPerson, "a flagged mesh outside the main frustum is still drawn, with the bit");
    check(fpCasts && fpCasts->firstPerson, "the cast flag still draws through the first-person projection");
    check(child && child->firstPerson, "a mesh under the first-person root is drawn with the bit, though it carries no flag");
    check(plain && !plain->firstPerson, "an unflagged mesh is drawn without it");
    const auto* after = dev.find(kPlainAfter);
    check(after && !after->firstPerson && !dev.fp, "the bit is reset after a flagged entity: the next draw has none, and the device is left clear");
    check(dev.find(kBehind) == nullptr, "an unflagged mesh outside the frustum is still culled");

    check(voxiPtr->submittedDrawCount() == dev.drawn.size() + 1, "every raster draw reached Voxi, and the culled mesh by the direct route (it still casts)");
    for (usize i = 0; i < voxiPtr->submittedDrawCount(); ++i) {
        const voxi::VoxiRenderer::SubmittedDraw d = voxiPtr->submittedDraw(i);
        if (d.mesh == kFp || d.mesh == kFpBehind || d.mesh == kChild) {
            check(d.firstPerson && d.noRay && !d.inShadowCascades() && !d.voxelised() && d.rtLane() == voxi::VoxiRenderer::RtLane::None,
                  "a first-person draw: no shadow, no ray, not voxelised (mesh " + std::to_string(d.mesh) + ")");
        } else if (d.mesh == kFpCasts) {
            check(d.firstPerson && d.inShadowCascades() && !d.voxelised() && d.rtLane() == voxi::VoxiRenderer::RtLane::OwnerHidden,
                  "with the cast flag it casts shadows and sits in the owner-hidden ray lane, still out of voxelisation");
        } else if (d.mesh == kPlain || d.mesh == kPlainAfter) {
            check(!d.firstPerson && !d.noRay && d.inShadowCascades() && d.voxelised() && d.rtLane() == voxi::VoxiRenderer::RtLane::Opaque,
                  "an unflagged draw is as before: it casts, voxelises and is in the opaque lane (mesh " + std::to_string(d.mesh) + ")");
        }
    }
    check(stats.drawn == 6 && stats.culled == 1, "six drawn, one culled");
}

static void testHooks() {
    AVER_INFO("=== the decide hook, the prepass, the direct route and a secondary camera ===");
    Scene s;
    const game::ViewCamera vc = makeCamera(s);
    s.place("fp behind", Vec3{-900, 0, 100}, 201, kFpBehind, scene::kRenderFirstPerson);
    s.place("plain", Vec3{600, 0, 100}, 202, kPlain);
    s.w.flush();

    struct Seen { bool fpIn = false, fpCulledIn = false, plainFp = true; };
    Seen seen;
    auto decide = [](game::EntityDecision& d, void* user) {
        auto* sn = static_cast<Seen*>(user);
        if (d.meshId == 201) { sn->fpIn = d.firstPerson; sn->fpCulledIn = d.frustumCulled; d.occlusionCulled = true; }   // a host's occlusion verdict
        if (d.meshId == 202) sn->plainFp = d.firstPerson;
    };
    {
        MockDevice dev;
        game::DrawWorldOptions opt;
        opt.suppressLog = true;
        opt.viewerPos = vc.eye; opt.haveViewerPos = true;
        opt.decide = decide; opt.user = &seen;
        game::SceneDrawStats stats;
        game::drawWorld(dev, vc.viewProj, s.content, stats, nullptr, nullptr, opt);
        check(seen.fpIn && !seen.fpCulledIn && !seen.plainFp, "the host is told which entity is first person, and that the main frustum did not cull it");
        check(dev.find(kFpBehind) != nullptr, "a host's occlusion verdict does not hide it: the opt-out is forced");
    }
    {
        MockDevice dev;
        game::DrawWorldOptions opt;
        opt.pass = game::DrawWorldPass::DepthPrepass;
        opt.suppressLog = true;
        opt.viewerPos = vc.eye; opt.haveViewerPos = true;
        game::SceneDrawStats stats;
        game::drawWorld(dev, vc.viewProj, s.content, stats, nullptr, nullptr, opt);
        bool plainSeen = false, fpSeen = false;
        for (rhi::MeshHandle m : dev.prepassed) { if (m == kPlain) plainSeen = true; if (m == kFpBehind) fpSeen = true; }
        check(plainSeen && !fpSeen, "the depth prepass skips a first-person mesh: its depth is the squeezed slab, written by the colour pass");
    }
    {
        // The direct route: the host drew it itself (colourAlreadyDrawn), so Voxi gets the submit and the device none.
        MockDevice dev;
        auto voxiPtr = std::make_unique<voxi::VoxiRenderer>();
        game::DrawWorldOptions opt;
        opt.suppressLog = true;
        opt.viewerPos = vc.eye; opt.haveViewerPos = true;
        opt.voxiRenderer = voxiPtr.get();
        opt.decide = [](game::EntityDecision& d, void*) { d.colourAlreadyDrawn = true; };
        game::SceneDrawStats stats;
        game::drawWorld(dev, vc.viewProj, s.content, stats, nullptr, nullptr, opt);
        bool fpDirect = false, plainDirect = false;
        for (usize i = 0; i < voxiPtr->submittedDrawCount(); ++i) {
            const auto d = voxiPtr->submittedDraw(i);
            if (d.mesh == kFpBehind) fpDirect = d.firstPerson && d.noRay && !d.inShadowCascades() && !d.voxelised();
            if (d.mesh == kPlain) plainDirect = !d.firstPerson && d.inShadowCascades() && d.voxelised();
        }
        check(dev.drawn.empty() && fpDirect && plainDirect, "on the direct route Voxi gets the same verdicts, and the device sees no draw");
    }
    {
        // Step 7: the direct route's host sink runs inside setDrawFirstPerson, as the raster route's draw does, so a feature fed there knows.
        MockDevice dev;
        struct Sink { MockDevice* dev; bool fpSeen = false, fpFlag = false, plainSeen = false, plainFlag = true; } sink{&dev};
        game::DrawWorldOptions opt;
        opt.suppressLog = true;
        opt.viewerPos = vc.eye; opt.haveViewerPos = true;
        opt.user = &sink;
        opt.decide = [](game::EntityDecision& d, void*) { d.colourAlreadyDrawn = true; };
        opt.onDirectDraw = [](rhi::MeshHandle m, const f32[16], const f32[4], f32, f32, rhi::BindingSetHandle, const void*, u32, bool, bool, void* user) {
            auto* k = static_cast<Sink*>(user);
            if (m == kFpBehind) { k->fpSeen = true; k->fpFlag = k->dev->fp; }
            if (m == kPlain) { k->plainSeen = true; k->plainFlag = k->dev->fp; }
        };
        game::SceneDrawStats stats;
        game::drawWorld(dev, vc.viewProj, s.content, stats, nullptr, nullptr, opt);
        check(sink.fpSeen && sink.fpFlag && sink.plainSeen && !sink.plainFlag && !dev.fp,
              "the direct sink sees the device's first-person state on for the first-person mesh only, and off afterwards");
    }
    {
        // A secondary camera's walk is not the player's view: the first-person mesh behind it is culled like any other.
        MockDevice dev;
        struct Sink { std::vector<rhi::MeshHandle> meshes; } sink;
        game::DrawWorldOptions opt;
        opt.pass = game::DrawWorldPass::View;
        opt.suppressLog = true;
        opt.viewerPos = vc.eye; opt.haveViewerPos = true;
        opt.user = &sink;
        opt.onViewDraw = [](rhi::MeshHandle m, const f32[16], const f32[4], f32, f32, rhi::BindingSetHandle, const void*, u32, bool, void* user) {
            static_cast<Sink*>(user)->meshes.push_back(m);
        };
        game::SceneDrawStats stats;
        game::drawWorld(dev, vc.viewProj, s.content, stats, nullptr, nullptr, opt);
        bool plainIn = false, fpIn = false;
        for (rhi::MeshHandle m : sink.meshes) { if (m == kPlain) plainIn = true; if (m == kFpBehind) fpIn = true; }
        check(plainIn && !fpIn, "a secondary camera's walk culls a first-person mesh by its own frustum");
    }
}

// Step 6: the cascades shadowPass renders. Raster: all of them, read by the frame. Ray tracing on: none, unless a ray-driven frame draws a
// first-person overlay, which gets the nearest one and reads it through its own constants (the frame's readers stay off).
static void testCascadePlan() {
    using V = voxi::VoxiRenderer;
    const V::CascadePlan raster = V::planCascades(4, false, false);
    check(raster.count == 4 && raster.mainReads, "raster frames render every fitted cascade and read them");
    const V::CascadePlan rasterFp = V::planCascades(4, false, true);
    check(rasterFp.count == 4 && rasterFp.mainReads, "a first-person mesh changes nothing in a raster frame");
    const V::CascadePlan rt = V::planCascades(4, true, false);
    check(rt.count == 0 && !rt.mainReads, "ray-traced frames without an overlay render no cascade");
    const V::CascadePlan rtFp = V::planCascades(4, true, true);
    check(rtFp.count == 1 && !rtFp.mainReads, "a first-person overlay gets the nearest cascade only, unread by the frame");
    const V::CascadePlan none = V::planCascades(0, true, true);
    check(none.count == 0, "no fitted cascade, nothing to render");
}

// Step 7: the path-traced reference leaves first-person draws out unless asked to include them.
static void testPathTracerExclusion() {
#if AVER_TEST_HAS_PT
    pt::PtSceneView view;
    const f32 world[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const f32 col[4] = {1, 1, 1, 1};
    view.submitDraw(kPlain, world, col, 0, 0.5f, 0, nullptr, 0, false);
    view.onDrawFirstPerson(true);
    view.submitDraw(kFp, world, col, 0, 0.5f, 0, nullptr, 0, false);
    view.onDrawFirstPerson(false);
    check(view.submittedDrawCount() == 1, "the path-traced view takes the plain draw and leaves the first-person one out");
    view.setIncludeFirstPerson(true);
    view.onDrawFirstPerson(true);
    view.submitDraw(kFp, world, col, 0, 0.5f, 0, nullptr, 0, false);
    view.onDrawFirstPerson(false);
    check(view.includesFirstPerson() && view.submittedDrawCount() == 2, "with setIncludeFirstPerson it traces the first-person draw too");
#endif
}

int main() {
    testWalk();
    testHooks();
    testCascadePlan();
    testPathTracerExclusion();
    if (g_failures) { AVER_ERROR("FirstPersonWalkTest: {} check(s) failed", g_failures); return 1; }
    AVER_INFO("FirstPersonWalkTest: all checks passed");
    return 0;
}
