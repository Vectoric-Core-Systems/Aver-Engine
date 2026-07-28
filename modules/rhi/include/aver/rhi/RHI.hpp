#pragma once
#include "aver/core/Types.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <cstddef>
#include <vector>

// Aver RHI — the single render-hardware abstraction every GPU consumer targets.
// Backends (D3D12/D3D11/Vulkan) implement these interfaces; a Null backend always
// exists as a fallback so the engine runs headless / on unsupported hardware.
namespace aver::rhi {

enum class Backend { Null, D3D12, D3D11, Vulkan };
const char* backendName(Backend b);

struct SwapchainDesc {
    void* windowHandle = nullptr; // HWND
    u32 width = 0;
    u32 height = 0;
    u32 bufferCount = 2;
};

class ISwapchain {
public:
    virtual ~ISwapchain() = default;
    virtual void present() = 0;
    virtual void resize(u32 width, u32 height) = 0;
    virtual u32 width() const = 0;
    virtual u32 height() const = 0;
};

// Interleaved mesh vertex: position + normal (engine space, cm) + UV0. Enough for lit, textured
// solid rendering; tangents/skin come with the asset mesh pipeline later. There is deliberately NO
// tangent field: nothing authored has one, and a zero tangent that normalize() turns into NaN is
// the documented 0x141 TDR failure mode on this hardware. The material shading derives its tangent
// frame from ddx/ddy of world position and UV instead.
struct MeshVertex {
    f32 px, py, pz;
    f32 nx, ny, nz;
    f32 u, v;
};

// Every consumer of this struct reads it through something that cannot be checked at compile time:
// the D3D12 input layout names offsets as literals, the mesh-shader path binds the buffer as a RAW
// root SRV whose element size comes solely from HLSL `struct MeshVtx`, and an input layout naming
// fewer elements than the buffer holds is legal D3D12. One byte of drift is silent in all three.
static_assert(sizeof(MeshVertex) == 32, "MeshVertex is the HLSL MeshVtx / kMeshInputLayout ABI");
static_assert(offsetof(MeshVertex, px) == 0,
              "position must stay first: the DXR BLAS description points at the vertex buffer base");

// MeshHandle / LineHandle are declared in RHIResources.hpp so feature modules can name geometry
// without pulling in the whole device interface.

// Line vertex: position + colour (unlit), for grids/gizmos/debug.
struct LineVertex {
    f32 px, py, pz;
    f32 r, g, b;
};

struct DeviceDesc {
    // Preference order; createDevice() returns the first compiled-in backend that
    // initialises, falling back to Null.
    Backend preferred[4] = {Backend::D3D12, Backend::D3D11, Backend::Vulkan, Backend::Null};
    u32 preferredCount = 4;
    bool enableDebug = false;
    // Prefer the software rasteriser (D3D12: WARP) over any hardware adapter. A development
    // switch: WARP reports genuinely different tiers from the installed GPU, so it exercises
    // fallback paths that a capability clamp cannot reach. It is slow — use few frames.
    bool useWarp = false;
};

// What the physical device can actually do. Queried once at init; consumers (e.g. the Voxi
// render module) use it to decide which quality settings are offerable vs greyed out, so the
// UI never advertises a feature the hardware cannot run.
struct DeviceCaps {
    u32 msaaMask = 1;            // bit N set => N samples supported (bits 1,2,4,8)
    u32 maxMsaaSamples = 1;      // highest supported sample count (1 = no MSAA)
    u32 rayTracingTier = 0;      // 0 = none, 10 = DXR 1.0, 11 = DXR 1.1
    bool computeShaders = false;
    bool typedUavLoads = false;      // needed for voxel radiance read-modify-write
    bool conservativeRaster = false; // needed for watertight voxelization
    u32 shaderModel = 50;            // 51 = SM 5.1, 60 = SM 6.0, 65 = SM 6.5, ...
    u32 meshShaderTier = 0;          // 0 = none, 1 = Tier 1 (D3D12 Ultimate)
    bool dxcAvailable = false;       // DXIL compiler present (needed for SM 6.x)
    u32 resourceBindingTier = 0;     // 0 = unknown, 1/2/3 = D3D12_RESOURCE_BINDING_TIER_N
};

// ----- Capability clamp (development only) -------------------------------------------------
//
// The engine has only ever run on one GPU, so every capability-gated fallback in it is a
// reasoned claim rather than a measured one. This clamps what the device REPORTS so those
// paths can be executed on the hardware that is actually here.
//
// Two properties make it safe to leave in the product build. Every field can only ever REDUCE
// a capability — `clampCaps` takes minimums and clears flags, never sets them — so no override
// can make the engine attempt something the hardware cannot do. And the clamp is applied once,
// where the backend finishes querying the hardware, so every consumer (the backend's own
// pipeline selection included) sees a single reduced device and nothing anywhere branches on
// "was this overridden".
struct CapsOverride {
    bool active = false;
    bool noRayTracing = false;
    bool noMeshShaders = false;
    bool noConservativeRaster = false;
    bool noTypedUavLoads = false;
    // Suppresses DXC entirely, so shaders go through FXC at SM 5.1. This one has to reach the
    // shader compiler as well as the caps, because `dxcAvailable` describes a DLL that is either
    // loaded or not — reporting false while still compiling DXIL would test nothing.
    bool noDxc = false;
    u32  maxShaderModel = 0;       // 0 = no ceiling; 51/60/65/66 pin the reported model
    u32  maxMsaaSamples = 0;       // 0 = no ceiling
    u32  maxResourceBindingTier = 0; // 0 = no clamp; 1 = report Tier 1
};

// Parses a comma-separated list: no-rt, no-ms, no-cons-raster, no-typed-uav, no-dxc,
// sm=<51|60|65|66>, msaa=<1|2|4|8>, tier1. Returns false (and logs) on an unrecognised token,
// so a typo in a test switch fails loudly instead of quietly testing the full-fat device.
bool setCapsOverride(const char* commaSeparatedList);
const CapsOverride& capsOverride();

// Applied by each backend at the end of its own capability query. Monotonically reducing.
void clampCaps(DeviceCaps& caps);

// Camera post-processing.
//
// Every field here is a property of the CAMERA looking at the scene, not of any surface in it —
// which is the same reason the shared prelude already owns fog, the tonemap and the gamma encode
// (see "camera / post" in RHIShaders.cpp). A material system that owned these would make every
// shading model reimplement them identically.
//
// The defaults WERE the identity — exposure 1, no bloom, no adaptation — so that a post chain
// nobody had switched on could not move a pixel the oracle measures. Eye adaptation is no longer
// among them, and the reason is worth stating: once the light transport became physically correct,
// scene radiance became a physical quantity with nothing mapping it onto a display range, and an
// exposure pinned at 1.0 stopped being neutral and started being one arbitrary stop. A capture run
// still turns adaptation off, which is where that constraint actually belongs.
struct PostSettings {
    // Linear multiplier on scene radiance, applied BEFORE the tonemap. Overridden every frame by
    // the adaptation when autoExposure is on.
    f32 exposure = 1.0f;

    // Bloom. Zero intensity does not weight the pyramid to nothing — it means no pyramid is built
    // and no pass is recorded, which is the difference between "off" and "on and invisible".
    //
    // ON by default, modestly. With eye adaptation choosing the stop, anything the scene contains
    // that is genuinely brighter than the exposed range -- the sun disk, a specular highlight, a sky
    // seen from inside a dark interior -- has nowhere to go but clipped white. Bloom is what carries
    // that energy back into the image as a halo instead of a flat plateau, and it is the difference
    // between a highlight reading as bright and reading as blown. Deterministic per frame, unlike
    // the adaptation, so it needs no capture-run exception.
    f32 bloomIntensity = 0.06f;
    // Luminance above which a pixel contributes, and the width of the soft knee below it. A hard
    // threshold makes bloom pop in and out as a highlight crosses it, which is far more visible in
    // motion than the halo itself.
    f32 bloomThreshold = 1.0f;
    f32 bloomKnee      = 0.5f;

    // Eye adaptation, from a luminance histogram of the frame. ON by default.
    //
    // It was off, on the reasoning that a temporal feedback loop makes a frame depend on the frames
    // before it and the pixel oracle compares single frames. That reasoning is still true and is now
    // handled where it belongs -- a capture run turns it off (see the sandbox) -- because the other
    // half of the trade turned out to matter more: once the light transport is physically correct,
    // scene radiance is a physical quantity and NOTHING was mapping it onto a display range. A dark
    // concrete arena under a bright sky is genuinely dark in radiance terms, and with exposure
    // pinned at 1.0 it renders as a black floor beside blown-out walls. Every real camera adapts;
    // an engine that does not is not neutral, it is stuck at one arbitrary stop.
    bool autoExposure   = true;
    f32  exposureMin    = 0.05f;   // clamps on the computed multiplier, not on scene luminance
    f32  exposureMax    = 8.0f;
    f32  exposureSpeed  = 3.0f;    // adaptation rate, in e-folds per second
    // The middle-grey the adaptation drives the frame's average luminance towards.
    f32  exposureKey    = 0.18f;
    // Fraction of the histogram discarded at each end before averaging. Without the low cut a dark
    // sky dominates the average and the whole image blows out; without the high cut one specular
    // highlight closes the aperture on the entire frame.
    f32  histogramLowPercent  = 0.30f;
    f32  histogramHighPercent = 0.85f;
};

// The sky, the sun and the air between them — the authored ones.
//
// This replaces setSky's five loose arguments. It is a struct for the same reason PostSettings is:
// the thing being described has a dozen knobs, and a signature with a dozen parameters is a
// signature where two floats get swapped and nobody notices until the sunset is the wrong colour.
//
// EVERY DEFAULT REPRODUCES WHAT THE ENGINE RENDERED BEFORE IT EXISTED. fogFalloff 0 collapses the
// height-fog integral back to the uniform distance fog exactly; atmosphereHeight 0.65 is the
// exponent the sky gradient already used; clouds are off. The oracle measures this scene, and a new
// authoring surface must not move a pixel until somebody authors something.
struct SkyAtmosphere {
    bool enabled = false;

    // ---- the dome ----
    f32 zenith[3]  = {0.24f, 0.45f, 0.85f};   // authored sRGB, decoded in the shader
    f32 horizon[3] = {0.72f, 0.83f, 0.95f};
    // How far up the dome the horizon band reaches. It is the EXPONENT on the zenith blend, so
    // smaller values push the pale band higher and read as a thicker, hazier atmosphere; larger
    // ones pull it down to a thin bright line and read as thin, high-altitude air.
    f32 atmosphereHeight = 0.65f;
    // What the world below the horizon reflects back into the lower half of the dome. Without it
    // the sky simply continues underneath the camera, which is visible the moment anything is
    // reflective or the camera is above terrain.
    //
    // groundBlend is how much of it replaces the sky down there, and it defaults to ONE.
    //
    // It was zero, which meant the dome was sky in every direction including straight down. That is
    // survivable while the fill light is dim; at its real strength it is not. Half of what a vertical
    // wall sees is below the horizon, so with no ground the whole scene is lit by nothing but a
    // saturated blue dome and every neutral surface turns blue -- which is exactly what a real
    // outdoor scene does NOT do, because the ground bounce is a large, warm, desaturating part of
    // the fill. Turning it on is not a preference; leaving it off was a missing light path.
    f32 groundAlbedo[3] = {0.24f, 0.23f, 0.21f};
    f32 groundBlend     = 1.0f;
    // Multiplier on the sky-hemisphere irradiance every surface receives. The sky IS the fill light,
    // and 1.0 means "as bright as the sky actually is".
    //
    // It was 0.28, which made the sky as a LIGHT 3.6x dimmer than the same sky as seen by the camera
    // -- the one object in the scene lit by a different sky from the one behind it. The consequence
    // was a diffuse/direct ratio of about 10% where a bright clear day is nearer 35%, so every
    // shadow crushed to near-black and every surface facing away from the sun lost its colour. That
    // is most of "too little lighting", and it was hidden for as long as the fog was thick enough to
    // fill the shadows back in with grey.
    f32 skyLightIntensity = 1.0f;

    // ---- the sun ----
    // The DIRECTION is the authoritative field, pointing TOWARD the light, and it does not have to
    // be normalised -- the shaders always have. Degrees are the editing form, not the stored one:
    // deriving the vector from angles every frame would push it through two transcendentals and back,
    // and the result differs from an authored vector in the last few bits. That is invisible to a
    // person and not at all invisible to a pixel-exact oracle, so the conversion happens where the
    // editing does. setSunAngles / sunAngles below are that conversion.
    //
    // A THREE-QUARTER KEY, from setSunAngles(48, 145). The number to keep in mind is not the
    // elevation but the angle between this vector and the camera: the previous default sat 172
    // degrees from the editor's view axis, which is eight degrees off being directly behind the
    // eye. That is on-camera flash. Every visible face was lit at once, every cast shadow fell into
    // the blind spot its own caster occupies, and no reflection vector could ever find the sun -- a
    // capture of it put 48% of the frame inside a single 16-level luminance band with nothing
    // brighter than 227/255. It read as a diagram rather than a photograph, and no amount of
    // tuning downstream of it could have helped.
    //
    // 145 degrees is not free choice: the editor has TWO default cameras looking in OPPOSITE
    // directions -- the placeholder scene at sandbox/src/SandboxApp.cpp:765 (bearing 225) and the
    // level-load framing at :3969 (bearing 45) -- so no azimuth is over the shoulder of both, and
    // the only band that serves both is the near-perpendicular one. This lands 118.8 degrees off
    // the level camera and 101.6 off the placeholder's. Move either camera and this wants
    // re-deriving with it.
    //
    // 48 degrees of elevation is chosen against the shadow bias rather than by eye. The cascade's
    // normal-offset (modules/render.voxi/src/VoxiRenderer.cpp:589 and VoxiShaders.hpp:121-129)
    // buys 1.5*(2 - sin el)*sin el texels of depth and needs 0.5*cot(el); that ratio is monotone
    // in elevation and goes under water at 25 degrees. 48 holds a 3.1x margin where the old 37.7
    // held 2.0x, and leaves 23.8 degrees before |z| > 0.95 flips the light basis at
    // VoxiRenderer.cpp:510, which has no hysteresis.
    f32 sunDirection[3] = {-0.5481f, 0.3838f, 0.7431f};
    f32 sunColor[3]     = {1.0f, 0.96f, 0.90f};
    // 3.0 because that is the factor the lit pass had hardcoded. Making it authored is the point:
    // the same number now scales the direct light, the GI injection and the sun disk, where before
    // the first had it, the second did not, and the third was a separate literal.
    f32 sunIntensity    = 3.0f;
    // Kelvin. 0 means "use sunColor as authored"; any other value overrides it with the blackbody
    // colour, which is how a sunset is authored honestly rather than by eye.
    f32 sunTemperatureK = 0.0f;
    // The real sun subtends about half a degree. It sets the disk's size in the sky AND, once the
    // shadow filter reads it, how quickly a shadow's edge softens with distance from its caster.
    f32 sunAngularDiameterDeg = 0.545f;

    // ---- the air ----
    // A TINT on the in-scattered sky, not a replacement for it -- see averFogInscatter. White means
    // "the air is the colour of the sky", which is what clear air is.
    f32 fogColor[3] = {1.0f, 1.0f, 1.0f};
    // Extinction per world unit (centimetres), at fogHeight.
    //
    // 4e-6 is LIGHT HAZE. The number is not taste: Koschmieder's law puts meteorological visibility
    // at 3.912 / extinction, so 4e-6 per cm is about 10 km -- a clear day with enough aerial
    // perspective to read depth. For reference: 1.7e-6 is a 23 km clear day, 7.8e-6 a 5 km haze,
    // 7.8e-5 actual fog.
    //
    // It was 2e-4, which is 196 m visibility. The WMO calls anything under 1 km fog, so the default
    // was a fog bank -- half the contrast gone by 35 m and 94% of it by 200 m, on every surface, in
    // every scene. That single constant was most of "the renderer does not look realistic".
    f32 fogDensity = 4e-6f;
    // World Z at which the density is exactly fogDensity, and how fast it thins going up. A falloff
    // of ZERO is uniform fog at fogDensity everywhere, which is what this engine had: a grey veil
    // that thickens with distance alone, so a distant mountain top is as hazy as the valley floor.
    // Any positive falloff gives the thing people actually mean by fog -- haze that pools low and
    // clears with altitude.
    f32 fogHeight  = 0.0f;
    f32 fogFalloff = 0.0f;
    f32 fogStart   = 0.0f;      // distance in front of the camera before any fog accumulates
    f32 fogMaxOpacity = 1.0f;   // so distance never fully erases the world

    // ---- clouds ----
    bool cloudsEnabled = false;
    f32  cloudCoverage = 0.45f;   // 0 clear, 1 overcast
    f32  cloudDensity  = 1.0f;
    f32  cloudBottom   = 150000.0f;   // world Z of the layer's base and top
    f32  cloudTop      = 280000.0f;
    f32  cloudScale    = 0.00002f;    // 1 / the width of one noise feature, in world units
    f32  cloudWind[2]  = {900.0f, 260.0f};   // world units per second
    f32  cloudTime     = 0.0f;               // accumulated seconds; the app owns the clock

    // Elevation above the horizon and azimuth as a bearing about +Z from +X, both in degrees --
    // what a person authoring a time of day actually thinks in. These WRITE and READ sunDirection;
    // there is no second stored copy to fall out of step with it.
    void setSunAngles(f32 elevationDeg, f32 azimuthDeg);
    void sunAngles(f32& elevationDeg, f32& azimuthDeg) const;
};

class IDevice {
public:
    virtual ~IDevice() = default;
    virtual Backend backend() const = 0;
    virtual const char* adapterName() const = 0;
    virtual DeviceCaps caps() const { return {}; }

    // Generic resource creation for render-feature modules. Returns nullptr on backends without
    // GPU support (Null / the D3D11 + Vulkan stubs), which is how a feature declines to initialise
    // instead of failing the engine.
    virtual IResourceFactory* resources() { return nullptr; }

    // Render-feature registration. NON-owning: the caller keeps the feature alive.
    virtual void addRenderFeature(IRenderFeature* f) { (void)f; }
    virtual void removeRenderFeature(IRenderFeature* f) { (void)f; }

    // Target formats a feature must match when building pipelines that draw into the scene.
    //
    // Read the contract, not the name: this is the SCENE colour target's format, which a backend
    // running a post chain does not present directly. The scene is linear radiance in an HDR format
    // and the swapchain holds the tonemapped result, so a feature that built its pipelines against
    // the presented format would fail at draw time with an RTV/PSO mismatch. The name is kept
    // because it is the one every backend and feature already binds to.
    virtual Format backbufferFormat() const { return Format::Unknown; }
    virtual Format depthFormat() const { return Format::Unknown; }

    // Anti-aliasing sample count. Changing it rebuilds the scene targets and every PSO, so it
    // is a real (if heavyweight) runtime setting. Returns false if the count is unsupported.
    virtual u32 sampleCount() const { return 1; }
    virtual bool setSampleCount(u32 samples) { (void)samples; return false; }
    virtual ISwapchain* createSwapchain(const SwapchainDesc& desc) = 0;
    virtual void beginFrame() = 0; // acquires + clears the current backbuffer
    virtual void endFrame() = 0;   // finalizes the frame's command list

    // Frame clear colour (linear RGBA, 0..1). Default no-op for backends without a target.
    virtual void setClearColor(f32 r, f32 g, f32 b, f32 a) { (void)r; (void)g; (void)b; (void)a; }

    // ---- vertical sync ----
    // ON by default. Turning it off needs the swapchain to have been CREATED able to tear, which is
    // decided once at creation and cannot be changed afterwards without rebuilding it -- so a
    // backend that cannot tear reports vsyncSupported() false and setVSync(false) is a no-op that
    // keeps presenting at the refresh rate. That is deliberately not silent-failure-shaped: the
    // caller can ask first and grey the control rather than offer a switch that does nothing.
    virtual void setVSync(bool on) { (void)on; }
    virtual bool vsync() const { return true; }
    // Whether turning vsync OFF is possible here (DXGI tearing support, a compositor that allows it).
    virtual bool vsyncCanDisable() const { return false; }

    // Confine scene rendering to a sub-rectangle of the backbuffer, in physical pixels with a
    // top-left origin. Used by the editor so the 3D view fills only the dockspace's central
    // node instead of the whole window. (0,0,0,0) = full backbuffer.
    virtual void setViewportRect(u32 x, u32 y, u32 w, u32 h) { (void)x; (void)y; (void)w; (void)h; }

    // ---- the scene as a texture ----
    //
    // Send the post chain's output to an offscreen texture instead of the backbuffer, so the UI can
    // DRAW the scene rather than having to leave a hole for it.
    //
    // The hole is the reason this exists. An editor that scissors the 3D into a transparent gap in
    // its dockspace can never make that gap a TAB: docking a window into a node stops the node being
    // empty, ImGui then paints the node's background, and the scene -- already on the backbuffer --
    // is covered. Measured, not assumed: the probe read editor grey instead of the scene. With the
    // scene in a texture the viewport becomes an ordinary image in an ordinary window, which can be
    // tabbed, split, floated or dragged like anything else.
    //
    // The texture is the FULL backbuffer size, not the viewport's. Sizing it to the panel would mean
    // destroying and recreating a render target the UI is sampling every time somebody drags a
    // splitter, and that needs a waitIdle -- a whole-GPU stall once a frame during a drag. The scene
    // still renders only into setViewportRect's sub-rectangle, and the caller draws that sub-rect by
    // its texture coordinates.
    virtual void setViewportToTexture(bool on) { (void)on; }
    virtual bool viewportToTexture() const { return false; }
    // The UI identifier for that texture, or 0 when the mode is off or unsupported. Same contract as
    // uiTextureId: a plain integer, so no UI type crosses into this header.
    virtual u64 viewportTextureId() { return 0; }

    // GPU self-test: clear a tiny offscreen target to `in` and read the pixel back into
    // `outRGBA`. Returns true if the read-back matches (proves the GPU path works). A
    // backend without real GPU support returns false.
    virtual bool selfTest(const f32 inRGBA[4], f32 outRGBA[4]) { (void)inRGBA; (void)outRGBA; return false; }

    // ----- Immediate lit-mesh rendering (Phase 3) -----
    // Upload a static mesh (positions+normals+indices); returns a handle (0 = failure).
    virtual MeshHandle createMesh(const MeshVertex* verts, u32 vertexCount,
                                  const u32* indices, u32 indexCount) {
        (void)verts; (void)vertexCount; (void)indices; (void)indexCount; return 0;
    }
    // Per-frame camera (row-major, row-vector viewProj = view*proj). invViewProj is used
    // to reconstruct world-space rays for the procedural sky.
    virtual void setCamera(const f32 viewProj[16], const f32 invViewProj[16], const f32 cameraPos[3]) {
        (void)viewProj; (void)invViewProj; (void)cameraPos;
    }
    // Read it back. A feature that has to fit its own frustum to the view — cascaded shadow maps
    // are the reason this exists — otherwise needs the app to push the same matrices a second time,
    // which is two sources of truth for one camera and a guarantee that one of them goes stale on
    // the frame somebody adds a camera shake. The backend already owns this state; it just never
    // offered it back. false when the backend has no camera to give. Any output may be null.
    virtual bool camera(f32 viewProj[16], f32 invViewProj[16], f32 cameraPos[3]) const {
        (void)viewProj; (void)invViewProj; (void)cameraPos; return false;
    }
    virtual void setLight(const f32 dirToLight[3], const f32 color[3], f32 ambient) { (void)dirToLight; (void)color; (void)ambient; }
    // The sky, the sun and the air; see SkyAtmosphere. It supersedes the five-argument setSky this
    // replaces, and it also supersedes setLight for the SUN: the direction is derived from the
    // authored elevation and azimuth, so the two cannot disagree about where the light is.
    virtual void setSkyAtmosphere(const SkyAtmosphere& s) { (void)s; }
    virtual SkyAtmosphere skyAtmosphere() const { return {}; }
    // Camera post-processing; see PostSettings. Pushed the same way the sky and the sun are,
    // because it is the same kind of state: what the camera does with the scene, not what the
    // scene contains.
    virtual void setPostProcess(const PostSettings& p) { (void)p; }
    virtual PostSettings postProcess() const { return {}; }
    // Record one draw of `mesh` with a world matrix (row-major), base colour, and PBR
    // metallic/roughness (0..1).
    virtual void drawMesh(MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                          f32 metallic, f32 roughness) {
        (void)mesh; (void)world; (void)baseColor; (void)metallic; (void)roughness;
    }

    // Per-draw binding table 1 and its b2 constant block, sticky until changed and consumed by
    // every subsequent drawMesh. Sticky rather than two more drawMesh arguments so drawMesh stays
    // narrow, the way the engine already states per-draw modes (setWireframe, setLineDepth). The
    // sticky value is still forwarded to a feature — through IRenderFeature::submitDraw, which the
    // backend hands it alongside the b1 block — so a replayed pass shades from the same surface.
    //
    // `constants` is COPIED; the caller may reuse its buffer immediately.
    virtual void setDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) {
        (void)set; (void)constants; (void)bytes;
    }
    // What beginFrame RESETS the above to. Without this, a draw issued without one would inherit
    // whatever the previous frame's last draw left bound — which reads on screen as one object
    // wearing another's surface, and only for the draws that forgot, so it looks like a content bug.
    // Set once by whoever owns table 1's contents; the identity/fallback set belongs here.
    virtual void setDefaultDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) {
        (void)set; (void)constants; (void)bytes;
    }

    // Unlit line geometry (grid, gizmos): per-vertex colour, drawn as a line list.
    virtual LineHandle createLineMesh(const LineVertex* verts, u32 count) { (void)verts; (void)count; return 0; }
    virtual void drawLines(LineHandle mesh, const f32 world[16]) { (void)mesh; (void)world; }

    // Mesh shader geometry path (D3D12 Ultimate: mesh-shader Tier 1 + SM 6.5). Replaces the
    // input-assembler vertex path for every draw, which is why it lives here and not in a render
    // feature: it is a property of how geometry reaches the rasteriser, not of any one effect.
    // Ignored when unavailable.
    //
    // Global illumination, shadows and ray tracing used to be declared alongside this. They are a
    // render feature's business now (see IRenderFeature in RHIResources.hpp) and the app configures
    // that feature directly, so no vocabulary they introduced survives in this interface.
    virtual void setMeshShaders(bool enabled) { (void)enabled; }
    virtual bool meshShadersActive() const { return false; }

    // Render subsequent meshes as wireframe until toggled off.
    virtual void setWireframe(bool on) { (void)on; }

    // Line depth testing. Default true (grids occlude behind geometry); set false to draw
    // subsequent lines as an always-on-top overlay (editor gizmos) until toggled back on.
    virtual void setLineDepth(bool testDepth) { (void)testDepth; }

    // Verification: capture the backbuffer pixel at (x,y) during the next presented
    // frame. Poll getCapture() afterward. Lets tests confirm objects actually rasterize.
    virtual void requestCapture(u32 x, u32 y) { (void)x; (void)y; }
    virtual bool getCapture(f32 outRGBA[4]) { (void)outRGBA; return false; }
    // Full captured frame (tight RGBA8, top-to-bottom) after a requestCapture completes.
    virtual bool getFrameImage(std::vector<u8>& outRGBA, u32& w, u32& h) { (void)outRGBA; (void)w; (void)h; return false; }

    // ----- In-window UI (Dear ImGui) -----
    // Initialise ImGui on this device for the given native window. Returns false if the
    // backend has no UI support. After init, build widgets between uiNewFrame() (called
    // by the engine after beginFrame) and endFrame() (which records the UI draw data).
    virtual bool uiInit(void* windowHandle) { (void)windowHandle; return false; }
    virtual void uiNewFrame() {}
    virtual void uiShutdown() {}
    virtual bool uiActive() const { return false; }
    virtual bool uiWantsMouse() const { return false; }    // true when the cursor is over UI
    virtual bool uiWantsKeyboard() const { return false; }

    // Make a texture drawable by the UI, returning the identifier the UI layer expects. It is a
    // plain integer here on purpose: the UI's own texture-handle type must not cross into the RHI
    // headers, or every consumer of this header acquires a dependency on the UI library.
    //
    // The descriptor is allocated once and cached on the texture, so calling this every frame is
    // free. Returns 0 where the backend hosts no UI, which the caller treats as "no image".
    virtual u64 uiTextureId(TextureHandle t) { (void)t; return 0; }
};

// Colour temperature in Kelvin to LINEAR sRGB, normalised so the brightest channel is 1: the result
// is a colour, and how bright it is belongs to whatever multiplies it. Clamped to 1000..15000 K,
// which spans candlelight to a clear blue sky.
void blackbodySrgb(f32 kelvin, f32 outRgb[3]);

IDevice* createDevice(const DeviceDesc& desc = {});
void destroyDevice(IDevice* device);

// ----- UI (Dear ImGui) window-message routing -----
// A backend that hosts ImGui registers a handler here; the platform Window forwards raw
// messages to uiWndProc so ImGui receives input without Platform depending on the UI.
using UiWndProcFn = bool (*)(void* hwnd, u32 msg, u64 wparam, i64 lparam);
void registerUiWndProc(UiWndProcFn fn);
bool uiWndProc(void* hwnd, u32 msg, u64 wparam, i64 lparam);

} // namespace aver::rhi
