// The host half of sub-viewports. See SubViewportHost.hpp for what a view is and is not.
#include "aver/game/SubViewportHost.hpp"

#include "aver/core/Log.hpp"
#include "aver/core/SubViewport.hpp"

#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
#include "aver/framework/framework_subview_abi.h"
#include "aver/game/GameContent.hpp"
#include "aver/game/GameRender.hpp"
#include "aver/game/SubViewportCamera.hpp"
#include "aver/render/preview/ActorPreview.hpp"
#include "aver/scene/World.hpp"
#if AVER_MODULE_PBR
#include "aver/pbr/Material.hpp"
#include "aver/pbr/MaterialGpu.hpp"
#include "aver/pbr/MaterialSystem.hpp"
#endif
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aver::game {

// The mapping is pure and does not need the scene, so it is outside the guard.
SceneViewLighting sceneViewLighting(const rhi::SkyAtmosphere& sky) {
    SceneViewLighting l;
    const Vec3 d = Vec3{sky.sunDirection[0], sky.sunDirection[1], sky.sunDirection[2]}.getSafeNormal();
    if (d.sizeSquared() > 0.5f) { l.sunDir[0] = d.x; l.sunDir[1] = d.y; l.sunDir[2] = d.z; }
    // The studio preview's key light (1.6) is calibrated to the engine's own 3.0 sun; scale from there, clamped so a dim or a blown-out sun stays readable.
    const f32 gain = 1.6f * std::min(4.0f, std::max(0.0f, sky.sunIntensity / 3.0f));
    for (int i = 0; i < 3; ++i) l.sunRadiance[i] = std::max(0.0f, sky.sunColor[i]) * gain;
    // The dome colours are authored sRGB; the fill is the mean of the horizon and the zenith in linear light, at the level the studio's default fill sits at.
    const f32 sl = std::max(0.0f, sky.skyLightIntensity);
    for (int i = 0; i < 3; ++i) {
        const f32 h = std::pow(std::max(0.0f, sky.horizon[i]), 2.2f);
        const f32 z = std::pow(std::max(0.0f, sky.zenith[i]), 2.2f);
        l.ambient[i] = 0.45f * sl * (0.5f * h + 0.5f * z);
    }
    return l;
}

#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK

namespace {

using render::preview::ActorPreview;
using render::preview::PreviewDraw;
using render::preview::SceneViewParams;

// A view with more draws than this stops collecting (and says so once): each draw is a CPU copy and a GPU draw call.
constexpr u32 kMaxViewDraws = 8192;

struct Collector {
    std::vector<PreviewDraw> draws;
    pbr::MaterialSystem* materials = nullptr;
    u32 dropped = 0;
    u32 translucent = 0;
};

f32 srgbToLinear(f32 c) { return std::pow(std::max(0.0f, c), 2.2f); }

// DrawWorldPass::View's sink: one planned draw becomes one flat-coloured preview draw.
void collectDraw(rhi::MeshHandle mesh, const f32 world[16], const f32 col[4], f32 metallic, f32 roughness, rhi::BindingSetHandle matSet,
                 const void* matConstants, u32 matBytes, bool translucent, void* user) {
    Collector& c = *static_cast<Collector*>(user);
    if (translucent) { ++c.translucent; return; }   // glass and water need the blended pass a secondary view does not have
    if (c.draws.size() >= kMaxViewDraws) { ++c.dropped; return; }
    PreviewDraw d;
    d.mesh = mesh;
    std::memcpy(d.world, world, sizeof d.world);
    // The look's colour is authored sRGB (1,1,1 for an authored material); the flat shader wants linear.
    for (int i = 0; i < 3; ++i) d.baseColor[i] = srgbToLinear(col[i]);
    d.baseColor[3] = 1.0f;
    d.metallic = metallic;
    d.roughness = roughness;
#if AVER_MODULE_PBR
    // An authored material: its average colour (factor times the mean of the base-colour map), the honest answer for something that cannot sample a texture.
    if (c.materials && matSet && matSet != c.materials->fallbackBindingSet()) {
        f32 avg[3];
        if (c.materials->averageBaseColor(matSet, avg)) for (int i = 0; i < 3; ++i) d.baseColor[i] *= avg[i];
    }
    if (matConstants && matBytes >= sizeof(pbr::MaterialConstants)) {
        const auto* mc = static_cast<const pbr::MaterialConstants*>(matConstants);
        for (int i = 0; i < 3; ++i) d.emissive[i] = std::max(0.0f, mc->emissiveFactor[i]);
    }
#else
    (void)matSet; (void)matConstants; (void)matBytes;
#endif
    c.draws.push_back(d);
}

} // namespace

struct SubViewportHost::View {
    i32 id = 0;
    std::string key;
    std::string rtName;     // the MaterialSystem runtime-texture name
    std::unique_ptr<ActorPreview> preview;
    u32 w = 0, h = 0;
    rhi::TextureHandle texture = 0;
    bool noCameraLogged = false;
    bool droppedLogged = false;
};

SubViewportHost::SubViewportHost() = default;
SubViewportHost::~SubViewportHost() { detach(); }

void SubViewportHost::attach(rhi::IDevice& device, TextureRetiredFn retired, void* retiredUser) {
    device_ = &device;
    retired_ = retired;
    retiredUser_ = retiredUser;
}

void SubViewportHost::configure(const SubViewportSettings& s) {
    aver_fw_subview_configure(s.maxViews, s.rendersPerFrame, s.maxPixelsMega);
}

void SubViewportHost::detach() {
    if (!device_) { views_.clear(); return; }
    for (auto& v : views_) retire(*v);
    views_.clear();   // each ActorPreview waits for the GPU and frees its targets
    device_ = nullptr;
    retired_ = nullptr;
    retiredUser_ = nullptr;
}

SubViewportHost::View* SubViewportHost::find(i32 id) {
    for (auto& v : views_) if (v->id == id) return v.get();
    return nullptr;
}

rhi::TextureHandle SubViewportHost::texture(i32 id) const {
    for (const auto& v : views_) if (v->id == id) return v->texture;
    return 0;
}

// Takes a view out of every consumer's hands before its target goes: the UI's cached binding set, the runtime texture the materials name, and the feature
// registration. The target itself is freed by the feature's destructor (after a GPU wait) or by its resize.
void SubViewportHost::retire(View& v) {
    if (retired_ && v.texture) retired_(v.texture, retiredUser_);
    if (device_ && v.preview) device_->removeRenderFeature(v.preview.get());
#if AVER_MODULE_PBR
    if (lastMaterials_ && !v.rtName.empty()) {
        lastMaterials_->clearRuntimeTexture(v.rtName);
        touchMaterialsUsing(v.rtName);
    }
#endif
    aver_fw_subview_host_set_texture(v.id, 0, 0, 0);
    v.texture = 0;
}

// Materials whose texture slots name `@rt:<runtimeName>` are rewritten by the next MaterialSystem::update().
void SubViewportHost::touchMaterialsUsing(const std::string& runtimeName) {
#if AVER_MODULE_PBR
    const std::string path = "@rt:" + runtimeName;
    pbr::MaterialLibrary& lib = pbr::MaterialLibrary::get();
    const u32 n = lib.count();
    std::vector<pbr::MaterialHandle> hit;
    for (u32 i = 0; i < n; ++i) {
        const pbr::MaterialHandle h = lib.at(i);
        const pbr::MaterialDesc* d = h ? lib.desc(h) : nullptr;
        if (!d) continue;
        for (u32 s = 0; s < pbr::kTextureSlotCount; ++s)
            if (d->textures[s].path == path) { hit.push_back(h); break; }
    }
    for (pbr::MaterialHandle h : hit) lib.touch(h);
#else
    (void)runtimeName;
#endif
}

// Tells the scripts (the texture handle) and the materials (the runtime texture) where the view's picture is.
void SubViewportHost::publish(View& v, pbr::MaterialSystem* ms) {
    v.texture = v.preview->colorTexture();
    aver_fw_subview_host_set_texture(v.id, v.texture, static_cast<i32>(v.w), static_cast<i32>(v.h));
#if AVER_MODULE_PBR
    if (ms && !v.rtName.empty()) {
        ms->setRuntimeTexture(v.rtName, v.texture);
        touchMaterialsUsing(v.rtName);
    }
#else
    (void)ms;
#endif
}

// The view's drawing feature, made on first use and rebuilt when the effective size changes.
SubViewportHost::View* SubViewportHost::ensure(i32 id, u32 w, u32 h) {
    if (!device_) return nullptr;
    View* v = find(id);
    if (!v) {
        char key[160] = {};
        if (aver_fw_subview_key(id, key, sizeof key) <= 0) return nullptr;
        auto nv = std::make_unique<View>();
        nv->id = id;
        nv->key = key;
        nv->rtName = SubViewRegistry::runtimeTextureName(key);
        nv->preview.reset(ActorPreview::create(*device_, w, h, /*studioChrome=*/false));   // a scene view draws no studio backdrop, grid or bounds
        if (!nv->preview) {
            AVER_WARN("[SubViewport] '{}': the view's targets could not be created ({}x{})", nv->key, w, h);
            return nullptr;
        }
        nv->w = w;
        nv->h = h;
        device_->addRenderFeature(nv->preview.get());
        views_.push_back(std::move(nv));
        v = views_.back().get();
        AVER_INFO("[SubViewport] view '{}' (id {}): {}x{} target created", v->key, id, w, h);
        return v;
    }
    if (v->w != w || h != v->h) {
        // The target is rebuilt: the UI and the materials let go of the old one first; resize() waits for the GPU and frees it.
        if (retired_ && v->texture) retired_(v->texture, retiredUser_);
        if (!v->preview->resize(w, h)) return v;   // kept at the old size; the view still draws
        v->w = w;
        v->h = h;
        AVER_INFO("[SubViewport] view '{}' resized to {}x{}", v->key, w, h);
    }
    return v;
}

void SubViewportHost::update(const Inputs& in) {
    lastRendered_ = 0;
    if (!device_) return;
    aver_fw_subview_tick();

    // Views the registry no longer has (a level change, a script's destroy) go first.
    for (usize i = 0; i < views_.size();) {
        i32 info[8];
        if (aver_fw_subview_info(views_[i]->id, info)) { ++i; continue; }
        retire(*views_[i]);
        views_.erase(views_.begin() + static_cast<std::ptrdiff_t>(i));
    }
    const i32 live = aver_fw_subview_count();
    if (live == 0 && views_.empty()) return;   // THE COST GUARD: no view, no work

    lastMaterials_ = in.materials;
    if (!loggedBudget_) {
        loggedBudget_ = true;
        AVER_INFO("[SubViewport] {} view(s) live; secondary views are drawn by the cheap raster feature (no GI, no shadows, flat material colours)", live);
    }
    for (auto& v : views_) v->preview->setEnabled(false);   // only this frame's planned views draw

    i32 ids[AVER_FW_SUBVIEW_ABSOLUTE_MAX] = {}, clears[AVER_FW_SUBVIEW_ABSOLUTE_MAX] = {};
    const i32 n = aver_fw_subview_plan(static_cast<i64>(in.frame), ids, clears, AVER_FW_SUBVIEW_ABSOLUTE_MAX);
    if (n == 0 || !in.content) return;

    const SceneViewLighting light = sceneViewLighting(in.sky ? *in.sky : rhi::SkyAtmosphere{});
    scene::World& world = scene::World::instance();
    Collector col;
    col.materials = in.materials;
    SceneDrawStats scratch;

    for (i32 j = 0; j < n; ++j) {
        const i32 id = ids[j];
        i32 hi[8];
        float clr[4];
        if (!aver_fw_subview_host_info(id, hi, clr)) continue;
        const u32 w = static_cast<u32>(std::max(hi[0], 1)), h = static_cast<u32>(std::max(hi[1], 1));
        View* v = ensure(id, w, h);
        if (!v) continue;
        const bool fresh = v->texture == 0 || v->w != w || v->h != h || v->preview->colorTexture() != v->texture;

        ViewCamera cam;
        if (!viewCameraOf(world, static_cast<scene::Entity>(static_cast<u32>(hi[3])), static_cast<f32>(w) / static_cast<f32>(h), cam)) {
            if (!v->noCameraLogged) {
                v->noCameraLogged = true;
                AVER_WARN("[SubViewport] '{}': its camera entity {} is gone; the view keeps its last picture", v->key, hi[3]);
            }
            if (fresh) publish(*v, in.materials);
            continue;
        }
        v->noCameraLogged = false;

        // The secondary walk: this camera's frustum and cull mask, nothing of the primary view's per-frame state.
        DrawWorldOptions opts;
        if (in.walk) {
            opts.softBodies = in.walk->softBodies;
            opts.hair = in.walk->hair;
        }
        opts.pass = DrawWorldPass::View;
        opts.cameraCullInv = cam.cullInv;
        opts.viewerPos = cam.eye;
        opts.haveViewerPos = true;
        opts.suppressLog = true;
        opts.onViewDraw = &collectDraw;
        opts.user = &col;
        col.draws.clear();
        col.dropped = 0;
        col.translucent = 0;
        drawWorld(*device_, cam.viewProj, *in.content, scratch, in.materials, in.skinning, opts);
        if (col.dropped && !v->droppedLogged) {
            v->droppedLogged = true;
            AVER_WARN("[SubViewport] '{}': more than {} draws in view; the rest are not drawn", v->key, kMaxViewDraws);
        }

        SceneViewParams sp;
        std::memcpy(sp.viewProj, &cam.viewProj.m[0][0], sizeof sp.viewProj);
        sp.eye[0] = cam.eye.x; sp.eye[1] = cam.eye.y; sp.eye[2] = cam.eye.z;
        for (int i = 0; i < 3; ++i) { sp.sunDir[i] = light.sunDir[i]; sp.sunRadiance[i] = light.sunRadiance[i]; sp.ambient[i] = light.ambient[i]; }
        const bool transparent = (hi[2] & 1) != 0;
        for (int i = 0; i < 3; ++i) sp.clearColor[i] = transparent ? 0.0f : clr[i];   // premultiplied: a transparent view clears to (0,0,0,0)
        sp.clearColor[3] = clr[3];
        sp.clearTarget = clears[j] != 0;
        sp.linearOutput = (hi[2] & 2) != 0;
        v->preview->setDrawList(col.draws);
        v->preview->setSceneView(sp);
        v->preview->setEnabled(true);
        if (fresh) publish(*v, in.materials);
        ++lastRendered_;
        ++rendered_;
    }

    const u64 deferred = static_cast<u64>(aver_fw_subview_deferred());
    if (deferred > deferredLogged_ && deferredLogged_ == 0) {
        AVER_WARN("[SubViewport] more views are due than the per-frame budget draws; the others wait their turn (RENDER.SUBVIEWRENDERS)");
    }
    deferredLogged_ = std::max(deferredLogged_, deferred);
}

u32 SubViewportHost::onScreen(OnScreenView* out, u32 max) const {
    u32 n = 0;
    for (const auto& v : views_) {
        if (n >= max || !out) break;
        float rect[4];
        i32 info[8];
        if (!v->texture || !aver_fw_subview_screen(v->id, rect) || !aver_fw_subview_info(v->id, info) || info[6] == 0) continue;
        out[n].texture = v->texture;
        out[n].x = rect[0]; out[n].y = rect[1]; out[n].w = rect[2]; out[n].h = rect[3];
        ++n;
    }
    return n;
}

#else  // no scene or no framework: the class exists so a host links, and does nothing

struct SubViewportHost::View {};
SubViewportHost::SubViewportHost() = default;
SubViewportHost::~SubViewportHost() = default;
void SubViewportHost::attach(rhi::IDevice&, TextureRetiredFn, void*) {}
void SubViewportHost::detach() {}
void SubViewportHost::configure(const SubViewportSettings&) {}
void SubViewportHost::update(const Inputs&) { lastRendered_ = 0; }
u32 SubViewportHost::onScreen(OnScreenView*, u32) const { return 0; }
rhi::TextureHandle SubViewportHost::texture(i32) const { return 0; }
SubViewportHost::View* SubViewportHost::find(i32) { return nullptr; }
SubViewportHost::View* SubViewportHost::ensure(i32, u32, u32) { return nullptr; }
void SubViewportHost::retire(View&) {}
void SubViewportHost::publish(View&, pbr::MaterialSystem*) {}
void SubViewportHost::touchMaterialsUsing(const std::string&) {}

#endif

} // namespace aver::game
