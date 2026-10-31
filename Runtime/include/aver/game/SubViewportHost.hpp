// The host half of sub-viewports (R5, https://github.com/Vectoric-Core-Systems/Fabric-Connector/blob/main/docs/plans/G4_RENDER.md): reads the views a script or a converted level created (framework_subview_abi.h),
// draws the ones the registry plans each frame into their own colour targets, and hands each target to the materials and the UI that name it.
//
// THE CHEAP CONFIGURATION, on purpose. A secondary view is NOT a second Voxi: Voxi's global illumination, ray-traced shadows, ReSTIR and NRD2 history are one set of
// per-renderer state, and a second camera running through them would corrupt the main view's history. A view is drawn by its own raster feature
// (Aver.Render.ActorPreview in scene-view mode): the world's meshes under one sun and a sky fill, flat per-material colours, no shadows, no GI, no post chain. It
// costs a world walk on the CPU and one draw per visible mesh on the GPU per view per frame, which is why the number of views and the number drawn per frame are
// budgeted. With no view in the registry update() returns at once and nothing is created.
#pragma once
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/rhi/RHI.hpp"

#include <memory>
#include <string>
#include <vector>

namespace aver::pbr { class MaterialSystem; }
namespace aver::render { class SkinnedScene; }
namespace aver::render::preview { class ActorPreview; }

namespace aver::game {

class GameContent;
struct DrawWorldOptions;

// The project's cost guard for sub-viewports (RENDER.SUBVIEWPORTS, RENDER.SUBVIEWRENDERS, RENDER.SUBVIEWPIXELS). The defaults are the framework ABI's.
struct SubViewportSettings {
    i32 maxViews = 4;            // live views; 0 turns the feature off
    i32 rendersPerFrame = 2;
    f32 maxPixelsMega = 8.0f;    // summed over live views, megapixels
};

// The project's keys (-1 = not stated) as the cost guard: views 0..16 (0 = off), renders per frame 1..16, pixel cap in megapixels (> 0).
inline SubViewportSettings subViewportSettingsOf(i32 viewports, i32 renders, f32 pixelsMega) {
    SubViewportSettings s;
    if (viewports >= 0) s.maxViews = viewports > 16 ? 16 : viewports;
    if (renders >= 1) s.rendersPerFrame = renders > 16 ? 16 : renders;
    if (pixelsMega > 0.0f) s.maxPixelsMega = pixelsMega > 256.0f ? 256.0f : pixelsMega;
    return s;
}

// The light a secondary view shades with, taken from the level's sky: one sun and a hemisphere fill. `sunRadiance` is the sun colour times a unit intensity the
// view's flat shading is calibrated to (1.6 for the engine's 3.0 sun, as the studio preview's key light), `ambient` the mean of the dome's two colours decoded to linear
// and scaled by the sky-light multiplier. Pure, so the mapping is a test.
struct SceneViewLighting {
    f32 sunDir[3] = {0.4f, 0.3f, 0.86f};
    f32 sunRadiance[3] = {1.6f, 1.6f, 1.6f};
    f32 ambient[3] = {0.26f, 0.30f, 0.36f};
};
SceneViewLighting sceneViewLighting(const rhi::SkyAtmosphere& sky);

class SubViewportHost {
public:
    SubViewportHost();
    ~SubViewportHost();
    SubViewportHost(const SubViewportHost&) = delete;
    SubViewportHost& operator=(const SubViewportHost&) = delete;

    // Called with a view's texture just before it is rebuilt or destroyed, so the UI renderer can forget its cached binding set (UiRenderer::dropBinding).
    using TextureRetiredFn = void (*)(rhi::TextureHandle texture, void* user);

    // The device the views draw on. No GPU object is made until a view needs one.
    void attach(rhi::IDevice& device, TextureRetiredFn retired = nullptr, void* retiredUser = nullptr);
    // Waits for the GPU and destroys every target. Safe to call twice.
    void detach();
    // Applies the project's budget to the registry (aver_fw_subview_configure).
    static void configure(const SubViewportSettings& s);

    struct Inputs {
        u64 frame = 0;                           // a counter that never goes backwards (the engine's frame number)
        GameContent* content = nullptr;
        pbr::MaterialSystem* materials = nullptr;    // the main renderer's: only read, for each material's average colour; null draws the look colours
        render::SkinnedScene* skinning = nullptr;
        const DrawWorldOptions* walk = nullptr;      // soft bodies and hair of the primary walk; the pass, camera and sinks are set here
        const rhi::SkyAtmosphere* sky = nullptr;     // the level's sun and sky; null uses the studio defaults
    };
    // Once a frame, BEFORE the device's beginFrame: the views' features draw in beginFrame (prePass), so their draw lists and cameras must be ready by then.
    void update(const Inputs& in);

    // A view shown over the window (aver_fw_subview_set_screen_rect) that has a picture.
    struct OnScreenView {
        rhi::TextureHandle texture = 0;
        f32 x = 0, y = 0, w = 0, h = 0;          // window pixels
    };
    // Fills `out` (at most `max`), returns the count. The host draws each as a textured rectangle (u0 v0 = 0, u1 v1 = 1) in its UI frame.
    u32 onScreen(OnScreenView* out, u32 max) const;

    u32 liveViews() const { return static_cast<u32>(views_.size()); }
    u64 viewsRendered() const { return rendered_; }          // pictures drawn since attach
    u32 lastFrameRendered() const { return lastRendered_; }  // pictures set up by the most recent update()
    // The target of a view, 0 if the host has none for it (yet).
    rhi::TextureHandle texture(i32 id) const;

private:
    struct View;
    View* find(i32 id);
    View* ensure(i32 id, u32 w, u32 h);
    void retire(View& v);
    void publish(View& v, pbr::MaterialSystem* ms);
    static void touchMaterialsUsing(const std::string& runtimeName);

    rhi::IDevice* device_ = nullptr;
    TextureRetiredFn retired_ = nullptr;
    void* retiredUser_ = nullptr;
    std::vector<std::unique_ptr<View>> views_;
    u64 rendered_ = 0;
    u32 lastRendered_ = 0;
    bool loggedBudget_ = false;
    u64 deferredLogged_ = 0;
    pbr::MaterialSystem* lastMaterials_ = nullptr;
};

} // namespace aver::game
