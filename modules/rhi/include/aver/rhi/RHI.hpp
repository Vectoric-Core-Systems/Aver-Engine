// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Aver RHI — the single render-hardware abstraction every GPU consumer targets. Backends
// (D3D12/D3D11/Vulkan) implement these interfaces; a Null backend is always available as a fallback.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/rhi/Atmosphere.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace aver::rhi {

// Which graphics API a device is built on.
enum class Backend { Null, D3D12, D3D11, Vulkan };
// Human-readable name for a backend.
const char* backendName(Backend b);

// Parses a backend name -- "d3d12", "d3d11", "vulkan", "null", case-insensitively. False when the
// name is not one of those, leaving `out` untouched, so a typo is a diagnosable error rather than a
// silent fallback to whatever happened to be first.
bool parseBackendName(const char* name, Backend& out);

// How to create a swapchain for a native window.
struct SwapchainDesc {
    void* windowHandle = nullptr; // HWND
    u32 width = 0;
    u32 height = 0;
    u32 bufferCount = 2;
};

// The presentable back buffers for one window.
class ISwapchain {
public:
    virtual ~ISwapchain() = default;
    virtual void present() = 0;
    virtual void resize(u32 width, u32 height) = 0;
    virtual u32 width() const = 0;
    virtual u32 height() const = 0;
};

// Interleaved mesh vertex: position + normal (engine space, cm) + UV0.
struct MeshVertex {
    f32 px, py, pz;
    f32 nx, ny, nz;
    f32 u, v;
};

// MeshVertex is the HLSL MeshVtx / kMeshInputLayout ABI; position must stay first for DXR BLAS.
static_assert(sizeof(MeshVertex) == 32, "MeshVertex is the HLSL MeshVtx / kMeshInputLayout ABI");
static_assert(offsetof(MeshVertex, px) == 0,
              "position must stay first: the DXR BLAS description points at the vertex buffer base");

// Line vertex: position + colour (unlit), for grids/gizmos/debug.
struct LineVertex {
    f32 px, py, pz;
    f32 r, g, b;
};

// How to create a device: backend preference order and development switches.
struct DeviceDesc {
    Backend preferred[4] = {Backend::D3D12, Backend::D3D11, Backend::Vulkan, Backend::Null};
    u32 preferredCount = 4;
    bool enableDebug = false;
    bool useWarp = false;   // prefer the software rasteriser (D3D12: WARP) over any hardware adapter
};

// What the physical device can actually do. Queried once at init.
struct DeviceCaps {
    u32 msaaMask = 1;            // bit N set => N samples supported (bits 1,2,4,8)
    u32 maxMsaaSamples = 1;      // highest supported sample count (1 = no MSAA)
    u32 rayTracingTier = 0;      // 0 = none, 10 = DXR 1.0, 11 = DXR 1.1
    bool computeShaders = false;
    bool typedUavLoads = false;
    bool conservativeRaster = false;
    u32 shaderModel = 50;            // 51 = SM 5.1, 60 = SM 6.0, 65 = SM 6.5, ...
    u32 meshShaderTier = 0;          // 0 = none, 1 = Tier 1 (D3D12 Ultimate)
    bool dxcAvailable = false;       // DXIL compiler present (needed for SM 6.x)
    u32 resourceBindingTier = 0;     // 0 = unknown, 1/2/3 = D3D12_RESOURCE_BINDING_TIER_N

    // Whether a shader may index a large texture array by a computed value -- ray-traced path only.
    bool rtBindlessTextures = false;

    // Whether a shader may perform 64-bit atomics on a buffer -- needed by a lock-free hash map.
    bool shaderInt64Atomics = false;

    // Frame features a backend provides (features gate on these, never on backend()):
    // compute dispatches, copies and barriers may be recorded inside the scene pass (Voxi's staged
    // ray-driven passes, NRD2 and NeuRaC record there);
    bool computeInScenePass = false;
    // the G-buffer is written and its getters answer (viewZ, normal-roughness, velocity, previous view-proj);
    bool gBuffer = false;
    // blended draws are replayed after the opaque scene (Voxi's ray-traced glass composite).
    bool blendedReplay = false;
};

// A development clamp on what a device REPORTS, so capability-gated fallback paths can be run on
// hardware that does not need them. Every field only ever reduces a capability.
struct CapsOverride {
    bool active = false;
    bool noRayTracing = false;
    bool noMeshShaders = false;
    bool noConservativeRaster = false;
    bool noTypedUavLoads = false;
    bool noDxc = false;            // suppresses DXC in the shader compiler too, not just the caps
    u32  maxShaderModel = 0;       // 0 = no ceiling; 51/60/65/66 pin the reported model
    u32  maxMsaaSamples = 0;       // 0 = no ceiling
    u32  maxResourceBindingTier = 0; // 0 = no clamp; 1 = report Tier 1
};

// Parses a comma-separated list: no-rt, no-ms, no-cons-raster, no-typed-uav, no-dxc,
// sm=<51|60|65|66>, msaa=<1|2|4|8>, tier1. Returns false and applies nothing on a bad token.
bool setCapsOverride(const char* commaSeparatedList);
// The active override.
const CapsOverride& capsOverride();

// Applied by each backend at the end of its own capability query. Monotonically reducing.
void clampCaps(DeviceCaps& caps);

// Simulated device loss, after this many presented frames. 0 (default) never fires.
void setSimulatedDeviceLoss(u32 afterPresentedFrames);
u32  simulatedDeviceLoss();

// D3D12 DRED (Device Removed Extended Data), forced on via --dred.
inline bool g_dredEnabled = false;
inline void setDredEnabled(bool enabled) { g_dredEnabled = enabled; }
inline bool dredEnabled() { return g_dredEnabled; }
// --gpu-validation: D3D12 GPU-based validation (checks every descriptor and resource state a shader
// actually touches). Needs --debug-layer as well, and is far slower; for diagnosing device loss only.
inline bool g_gpuValidationEnabled = false;
inline void setGpuValidationEnabled(bool enabled) { g_gpuValidationEnabled = enabled; }
inline bool gpuValidationEnabled() { return g_gpuValidationEnabled; }

// Engine radiance units -> cd/m^2. One engine radiance unit is 100000/3 cd/m^2.
constexpr f32 kLuminanceToCdm2 = 100000.0f / 3.0f;

// Camera post-processing: exposure, bloom and eye adaptation.
struct PostSettings {
    // Linear multiplier on scene radiance, applied BEFORE the tonemap.
    f32 exposure = 1.0f;

    // Bloom. Zero intensity builds no pyramid and records no pass at all.
    f32 bloomIntensity = 0.06f;
    f32 bloomThreshold = 4.0f;
    f32 bloomKnee      = 0.5f;

    // Eye adaptation, from a luminance histogram of the frame.
    bool autoExposure   = true;
    f32  exposureMin    = 0.01f;
    f32  exposureMax    = 256.0f;
    f32  exposureSpeed     = 3.0f;   // toward a brighter view (exposure falling)
    f32  exposureSpeedDark = 0.5f;   // toward a darker view (exposure rising), e-folds/s

    // Perceptual eye adaptation (Krawczyk, Myszkowski & Seidel 2005). [0,1]: 0 = full adaptation;
    // 1 = perceptual model. Implemented in post.hlsl's CSExposure.
    f32  adaptationRealism = 1.0f;   // [0,1]

    // Scotopic night vision (Krawczyk et al.). [0,1]: below ~1 cd/m^2 rods take over, draining colour.
    f32  nightVision = 1.0f;   // [0,1]

    // Centre-weighted metering (console post.meteringCenterWeight). [0,1]: weights the centre up to
    // 4x an edge pixel. Implemented in post.hlsl's CSHistogram.
    f32  meteringCenterWeight = 0.5f;   // [0,1]

    // Target brightness -- the log-average of the histogram's middle band.
    f32  exposureKey    = 0.125f;
    // Fraction of the histogram discarded at each end before averaging.
    f32  histogramLowPercent  = 0.10f;
    f32  histogramHighPercent = 0.90f;

    // Which tone curve. 0 = original per-channel Narkowicz/Hill; 1 = ACES with matrices;
    // 2 = acesLumaTonemap (tonemaps luminance, restores chromaticity).
    u32  tonemap = 1;

    // Ceiling on scene radiance immediately before the tonemap; 0 disables it.
    f32  maxRadiance = 8.0f;

    // Local exposure. [0,1]: fraction of deviation from middle grey removed. Both ride PostCB.clampRadiance.
    f32  localExposureShadows    = 0.0f;
    f32  localExposureHighlights = 0.0f;
};

// Field by field, not memcmp: the bool leaves padding whose bytes a copy need not preserve. The size
// check is the reminder -- a new PostSettings field changes it, and must be added here too.
inline bool postSettingsEqual(const PostSettings& a, const PostSettings& b) {
    static_assert(sizeof(PostSettings) == 76, "a PostSettings field was added: compare it below too");
    return a.exposure == b.exposure && a.bloomIntensity == b.bloomIntensity &&
           a.bloomThreshold == b.bloomThreshold && a.bloomKnee == b.bloomKnee &&
           a.autoExposure == b.autoExposure && a.exposureMin == b.exposureMin &&
           a.exposureMax == b.exposureMax && a.exposureSpeed == b.exposureSpeed &&
           a.exposureSpeedDark == b.exposureSpeedDark &&
           a.adaptationRealism == b.adaptationRealism && a.nightVision == b.nightVision &&
           a.meteringCenterWeight == b.meteringCenterWeight &&
           a.exposureKey == b.exposureKey && a.histogramLowPercent == b.histogramLowPercent &&
           a.histogramHighPercent == b.histogramHighPercent && a.tonemap == b.tonemap &&
           a.maxRadiance == b.maxRadiance &&
           a.localExposureShadows == b.localExposureShadows &&
           a.localExposureHighlights == b.localExposureHighlights;
}

// Which sky the engine draws. Authored is a two-colour dome; Physical derives the dome, the direct
// sun's colour and the aerial perspective from Rayleigh/Mie/ozone scattering.
enum class SkyModel : u32 { Authored = 0, Physical = 1 };

// The sky, the sun and the air between them, as a level authors them.
struct SkyAtmosphere {
    bool enabled = false;

    // Physical is the DEFAULT. The dome, its exponent and the sun's colour are then derived from the
    // sun's elevation and are inert as authored values; --sky-authored restores them.
    SkyModel          model = SkyModel::Physical;
    AtmosphereProfile air{};   // only read when model is Physical

    // ---- the dome ----
    f32 zenith[3]  = {0.24f, 0.45f, 0.85f};   // authored sRGB, decoded in the shader; Authored only
    f32 horizon[3] = {0.72f, 0.83f, 0.95f};
    f32 atmosphereHeight = 0.65f;   // exponent on the horizon-to-zenith blend
    // What the world below the horizon reflects back into the lower half of the dome.
    f32 groundAlbedo[3] = {0.24f, 0.23f, 0.21f};
    f32 groundBlend     = 1.0f;   // how much of the ground replaces the sky below the horizon
    // Multiplier on the sky-hemisphere irradiance every surface receives.
    f32 skyLightIntensity = 1.0f;

    // ---- the sun ----
    f32 sunDirection[3] = {-0.5481f, 0.3838f, 0.7431f};   // toward the light
    f32 sunColor[3]     = {1.0f, 1.0f, 1.0f};
    f32 sunIntensity    = 3.0f;
    f32 sunTemperatureK = 0.0f;   // Kelvin; 0 means use sunColor as authored
    f32 sunAngularDiameterDeg = 0.545f;

    // White furnace radiance. 0 = off. Non-zero replaces sky, ground and sun with a uniform
    // environment -- the standard energy-conservation test.
    f32 furnaceRadiance = 0.0f;

    // Keeps the sun on inside the furnace, with the uniform environment at zero. Exercises the
    // ambient term only; furnace without this is blind to missing /PI in sunlit reflections.
    bool furnaceSun = false;

    // ---- the air ----
    f32 fogColor[3] = {1.0f, 1.0f, 1.0f};   // a TINT on the in-scattered sky, not a replacement
    f32 fogDensity = 4e-6f;     // extinction per world unit (cm), at fogHeight
    f32 fogHeight  = 0.0f;      // world Z at which density is exactly fogDensity
    f32 fogFalloff = 0.0f;      // how fast it thins going up; 0 is uniform fog
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
    // Which sky this is: two levels with different seeds get different cloud fields from the same
    // settings. 0 is the unseeded field and reproduces previous output exactly.
    i32  cloudSeed     = 0;

    // Writes sunDirection from an elevation above the horizon and an azimuth bearing about +Z
    // from +X, both in degrees.
    void setSunAngles(f32 elevationDeg, f32 azimuthDeg);
    // Reads sunDirection back as elevation and azimuth in degrees.
    void sunAngles(f32& elevationDeg, f32& azimuthDeg) const;
};

// One node of a per-pass GPU timing report -- the public mirror of D3D12Device's private GpuAccum
// tree. `ms` is INCLUSIVE (itself + everything nested); subtract direct children's ms for the
// pass's own time. Flat and parent-indexed for O(n) traversal.
struct GpuTimingNode {
    // Sentinel for "top-level", matching D3D12Device's private kNoAccumParent.
    static constexpr u32 kNoParent = 0xFFFFFFFFu;
    std::string label;
    f64 ms = 0;              // inclusive, averaged across framesAccumulated frames
    u32 parent = kNoParent;  // index into the SAME report's `nodes`, or kNoParent
};

// A snapshot of one device's per-pass GPU timing, as of the last frame it collected one.
// `supported` is the CAPABILITY axis: false means this backend cannot report timings at all.
// `nodes` empty with `supported` true is the CONTENT axis: enabled but nothing accumulated yet.
struct GpuTimingReport {
    bool supported = false;
    // Averaged over this many frames since boot, or since the last resetGpuTiming().
    u32 framesAccumulated = 0;
    std::vector<GpuTimingNode> nodes;
};

// A snapshot of the adapter's video memory budget and usage, split as both backends' own APIs
// split it: LOCAL is memory on the GPU's own bus (VRAM discrete, whole pool on UMA); NON_LOCAL is
// everything else spillable (system memory over PCIe on discrete, unused on UMA).
// `supported`: false means the backend could not answer this call, and every numeric field is then 0.
struct VideoMemoryInfo {
    bool supported = false;
    u64  localBudgetBytes = 0;      // D3D12 DXGI_MEMORY_SEGMENT_GROUP_LOCAL Budget / Vulkan sum of heapBudget
    u64  localUsageBytes = 0;       // ...CurrentUsage / sum of heapUsage over DEVICE_LOCAL heaps
    u64  nonLocalBudgetBytes = 0;   // ...NON_LOCAL Budget / sum over every other heap
    u64  nonLocalUsageBytes = 0;    // ...NON_LOCAL CurrentUsage / sum over every other heap
};

// One GPU device: frame loop, scene state, immediate drawing, capture and in-window UI.
class IDevice {
public:
    virtual ~IDevice() = default;
    virtual Backend backend() const = 0;
    virtual const char* adapterName() const = 0;
    virtual DeviceCaps caps() const { return {}; }

    // Generic resource creation for render-feature modules. nullptr on backends without GPU support.
    virtual IResourceFactory* resources() { return nullptr; }

    // The SAME context drawMesh() uses internally so a registered IRenderFeature can override scene
    // draws. Exposed so a CALLER too can interleave setPipeline/dispatchMeshClusters with drawMesh().
    virtual IRenderContext* renderContext() { return nullptr; }

    // Render-feature registration. NON-owning: the caller keeps the feature alive.
    virtual void addRenderFeature(IRenderFeature* f) { (void)f; }
    virtual void removeRenderFeature(IRenderFeature* f) { (void)f; }

    // The upscaler turning scene-resolution colour into the present-resolution image, or null for none.
    // Non-owning like addRenderFeature: caller keeps it alive.
    virtual void setUpscaler(IUpscaler* u) { (void)u; }
    virtual IUpscaler* upscaler() const { return nullptr; }

    // ---- frame interpolation (docs/rendering/NEURAFI.md) ----
    // The generator is installed like the upscaler (non-owning, host-composed). Presents a generated
    // frame before every real one whenever it can: the G-buffer written and a valid previous frame.
    // Defaulted no-ops: a backend without it compiles unchanged.
    virtual void setFrameInterpolator(IFrameInterpolator* g) { (void)g; }
    virtual void setFrameInterpolation(bool on) { (void)on; }
    virtual bool frameInterpolation() const { return false; }
    // Called by the renderer roughly halfway through its frame's GPU work. With frame interpolation
    // the previous frame's real image is presented there, queue-ordered behind the work before it.
    virtual void frameMidpoint() {}
    // The refresh rate of the display the swapchain is on, Hz; 0 when unknown or headless.
    virtual f32 displayRefreshRate() const { return 0.0f; }
    // Diagnostics: captures (screenshots, --frames) take the GENERATED image instead of the real one.
    virtual void setFrameInterpCaptureGenerated(bool on) { (void)on; }
    // Visualisation: both presented images show the GENERATED one, for inspection.
    virtual void setFrameInterpShowGeneratedOnly(bool on) { (void)on; }
    // True when the LAST endFrame presented a generated frame ahead of the real one.
    virtual bool frameInterpolated() const { return false; }
    // A discontinuity the next frame must not be interpolated across: level load, respawn, teleport.
    virtual void noteSceneCut() {}

    // The SCENE colour target's format, which a backend running a post chain does not present directly.
    virtual Format backbufferFormat() const { return Format::Unknown; }
    virtual Format depthFormat() const { return Format::Unknown; }

    // Anti-aliasing sample count. Changing it rebuilds the scene targets and every PSO. setter returns false if unsupported.
    virtual u32 sampleCount() const { return 1; }
    virtual bool setSampleCount(u32 samples) { (void)samples; return false; }
    // Creates the swapchain for a native window.
    virtual ISwapchain* createSwapchain(const SwapchainDesc& desc) = 0;
    virtual void beginFrame() = 0; // acquires + clears the current backbuffer
    virtual void endFrame() = 0;   // finalizes the frame's command list

    // Records `record` into a private command list/buffer, submits it and waits for completion. Only when no frame is open. For tests and tools; never during a frame. False when unsupported or a frame is open.
    virtual bool runStandaloneCompute(const std::function<void(IRenderContext&)>& record) { (void)record; return false; }

    // Frame clear colour (linear RGBA, 0..1).
    virtual void setClearColor(f32 r, f32 g, f32 b, f32 a) { (void)r; (void)g; (void)b; (void)a; }

    // ---- vertical sync ----
    // Tearing has to be enabled when the swapchain is created, so a backend that cannot tear
    // reports vsyncCanDisable() false and setVSync(false) is a no-op.
    virtual void setVSync(bool on) { (void)on; }
    virtual bool vsync() const { return true; }
    virtual bool vsyncCanDisable() const { return false; }

    // Confines scene rendering to a sub-rectangle of the backbuffer, in physical pixels with a
    // top-left origin. (0,0,0,0) = full backbuffer.
    virtual void setViewportRect(u32 x, u32 y, u32 w, u32 h) { (void)x; (void)y; (void)w; (void)h; }

    // The aspect ratio scene rendering is confined to: the sub-rect above if set, else the whole
    // scene target. A RATIO not the rect: it's what a camera-deriving consumer needs.
    // 0 = "not known yet", read as "make no correction".
    virtual f32 viewportAspect() const { return 0.0f; }

    // Decouples the scene's render targets from the swapchain's: scene renders at round(present * scale),
    // post-chain composite upscales back to present size. Clamped [0.25, 1.0]; 1.0 default reproduces
    // 1:1 sizing exactly. Deferred to the next beginFrame() when a swapchain exists.
    virtual void setRenderScale(f32 scale) { (void)scale; }
    virtual f32  renderScale() const { return 1.0f; }

    // Sends the post chain's output to an offscreen texture instead of the backbuffer.
    virtual void setViewportToTexture(bool on) { (void)on; }
    virtual bool viewportToTexture() const { return false; }
    // The UI identifier for that texture, or 0 when the mode is off or unsupported.
    virtual u64 viewportTextureId() { return 0; }

    // Play in New Window: every presented image's viewport rect (after post and overlays, before the
    // editor UI) is also presented to a second window, `width` x `height`. A null handle removes it.
    // Calling again with the same handle resizes. False when unsupported or on failure.
    virtual bool setMirrorWindow(void* windowHandle, u32 width, u32 height) {
        (void)windowHandle; (void)width; (void)height; return false;
    }

    // True once this device has been REMOVED and can no longer execute anything. One-way and sticky:
    // nothing here recovers a lost device. Defaults to false so a backend that can't lose its device
    // is unaffected.
    virtual bool deviceLost() const { return false; }

    // True when some registered feature has taken the scene over. Lets a CALLER skip a draw entirely.
    // Defaults to false so a backend with no features is unaffected.
    virtual bool sceneSuppressed() const { return false; }

    // Per-pass GPU timing without going through the periodic AVER_INFO log. Two frames old on purpose:
    // timestamps are resolved from a readback slice only readable once the GPU has caught up.
    // Returned by value, not a reference: source data mutates every beginFrame.
    // Defaults to an unsupported/empty report so Vulkan, D3D11, Null, and every test mock are unaffected.
    virtual GpuTimingReport gpuTiming() const { return {}; }

    // Throws away what gpuTiming() has accumulated, so the next report averages only the frames from now on.
    // Defaulted to a no-op for the same reason gpuTiming() defaults to unsupported.
    virtual void resetGpuTiming() {}

    // GPU self-test: clears a tiny offscreen target to `in` and reads the pixel back into `outRGBA`.
    virtual bool selfTest(const f32 inRGBA[4], f32 outRGBA[4]) { (void)inRGBA; (void)outRGBA; return false; }

    // Uploads a static mesh (positions+normals+indices). Returns a handle, 0 on failure.
    virtual MeshHandle createMesh(const MeshVertex* verts, u32 vertexCount,
                                  const u32* indices, u32 indexCount) {
        (void)verts; (void)vertexCount; (void)indices; (void)indexCount; return 0;
    }

    // Polls the adapter's current video memory budget and usage. Defaults to an unsupported, all-zero
    // report so D3D11, Null, and every test mock compile and behave unchanged without implementing this.
    virtual VideoMemoryInfo videoMemory() const { return {}; }

    // Chooses which GPU heap createMesh() uploads to for calls AFTER this one. Default false =
    // BufferKind::Upload (CPU-visible; correct everywhere, but every draw/shadow/etc re-fetches it
    // on discrete cards). True moves new meshes to the Default heap, trading a one-shot sync copy
    // for that traffic. Permanent default on D3D11/Null/mocks.
    virtual void setStaticMeshHeapDefault(bool onDefaultHeap) { (void)onDefaultHeap; }
    virtual bool staticMeshHeapDefault() const { return false; }

    // Creates a mesh SHARING `source`'s vertex buffer with its own index buffer -- inverse of
    // createSkinTargetMesh's split below, for an LOD ladder. REFCOUNTED across every sharer.
    // `source` must be alive and not compute-written. Bounds are copied from `source`.
    // 0 on any refusal; caller MUST fall back to createMesh with its own full vertex array.
    virtual MeshHandle createMeshSharingVertices(MeshHandle source, const u32* indices, u32 indexCount) {
        (void)source; (void)indices; (void)indexCount; return 0;
    }

    // Releases a mesh's GPU memory. False if the handle is invalid, already dead, or still shared.
    // Handle NOT recycled: the slot is cleared and kept, so a stale handle addresses a dead mesh.
    virtual bool destroyMesh(MeshHandle mesh) { (void)mesh; return false; }

    // Creates a mesh whose VERTEX BUFFER IS A COMPUTE TARGET, sharing `source`'s index buffer.
    // Returned buffer is where compute writes rhi::MeshVertex elements, SEEDED with `source`'s
    // vertices so an unposed draw shows the bind pose. Zero on failure; `outVertices` then untouched.
    virtual MeshHandle createSkinTargetMesh(MeshHandle source, BufferHandle* outVertices) {
        (void)source; (void)outVertices; return 0;
    }

    // A POSED PART: a mesh SHARING a compute-written mesh's vertex buffer with its OWN index buffer.
    // REQUIRES a compute-written source, range-checks every index, and marks the result compute-written
    // too. Refcounted the same way: destroyMesh on the source is refused while a part lives.
    // 0 on refusal; caller keeps the whole-mesh draw.
    virtual MeshHandle createPosedPartMesh(MeshHandle posedSource, const u32* indices, u32 indexCount) {
        (void)posedSource; (void)indices; (void)indexCount; return 0;
    }

    // Non-zero (the buffer they live in) when a mesh's vertices are WRITTEN BY COMPUTE rather than
    // uploaded once; zero for every ordinary mesh. Lets a consumer caching derived values know its
    // cache expires every frame.
    virtual BufferHandle meshVertexBuffer(MeshHandle mesh) const { (void)mesh; return 0; }

    // A mesh's geometry as BUFFERS a shader can get descriptors over, plus element counts. False
    // when the backend can't express it -- the signal to fall back.
    virtual bool meshGeometry(MeshHandle mesh, BufferHandle* vb, BufferHandle* ib,
                              u32* vertexCount, u32* indexCount) const {
        (void)mesh; (void)vb; (void)ib; (void)vertexCount; (void)indexCount; return false;
    }
    // Mesh's LOCAL-SPACE bounding sphere (centre/radius before any world transform). Conservative,
    // not tight. False when the backend has no bounds, signalling skip culling.
    // LOCAL-space axis-aligned extents. The sphere is right for a frustum cull but wrong for
    // containment: use the AABB for point-in-volume or "where's the top" questions instead.
    virtual bool meshBoundsAabb(MeshHandle mesh, f32 outMin[3], f32 outMax[3]) const {
        (void)mesh; (void)outMin; (void)outMax; return false;
    }
    virtual bool meshBounds(MeshHandle mesh, f32 outCentre[3], f32* outRadius) const {
        (void)mesh; (void)outCentre; (void)outRadius; return false;
    }
    // Per-frame camera (row-major, row-vector viewProj = view*proj). invViewProjRel is the inverse
    // of the SAME view*proj with the view's translation removed, mapping clip space to world-space
    // OFFSET FROM cameraPos -- precise however far the camera is from the world origin.
    virtual void setCamera(const f32 viewProj[16], const f32 invViewProjRel[16], const f32 cameraPos[3]) {
        (void)viewProj; (void)invViewProjRel; (void)cameraPos;
    }
    // Reads the camera back, for a feature fitting its own frustum to the view. False when the
    // backend has no camera to give. Any output may be null.
    virtual bool camera(f32 viewProj[16], f32 invViewProjRel[16], f32 cameraPos[3]) const {
        (void)viewProj; (void)invViewProjRel; (void)cameraPos; return false;
    }
    // Holds the temporal-AA jitter at zero from the next uploaded frame (NRD2's training capture holds a
    // still pose on one sample position). Backends without jitter ignore it.
    virtual void setJitterSuppressed(bool on) { (void)on; }
    // This frame's temporal-AA jitter in scene pixels, as the uploaded camera carries it ((0, 0) = none).
    // False, `out` untouched: unknown (backends without jitter reporting).
    virtual bool taaJitter(f32 out[2]) const { (void)out; return false; }
    // The scene's own viewport rect in target pixels -- {x, y, w, h} -- for a feature reprojecting
    // a screen-space position between frames. NOT necessarily the whole render target: the editor docks
    // the 3D view in a sub-rect of the backbuffer. False when the backend has no viewport to give.
    virtual bool sceneViewport(f32 rect[4]) const { (void)rect; return false; }
    // Sets the directional light and the ambient term.
    virtual void setLight(const f32 dirToLight[3], const f32 color[3], f32 ambient) { (void)dirToLight; (void)color; (void)ambient; }
    // Sets what waves (water surface). Up to 3 entries of {dirX, dirY, k (rad/cm), speed (rad/s)}.
    // `count` 0 disables the surface. Caller may generate these however it likes.
    virtual void setWaterWaves(const f32 (*waves)[4], u32 count, f32 amplitude) {
        (void)waves; (void)count; (void)amplitude;
    }
    // Engine clock, forwarded into PerFrameCB::time so any shader can animate. Separate from
    // SkyAtmosphere::cloudTime -- a paused sky must not freeze animated materials. `seconds` is raw
    // and monotonic; the backend wraps it as the shader needs.
    virtual void setFrameTime(f32 seconds, f32 deltaSeconds) { (void)seconds; (void)deltaSeconds; }
    // Sets the sky, the sun and the air. Supersedes setLight for the sun.
    virtual void setSkyAtmosphere(const SkyAtmosphere& s) { (void)s; }
    virtual SkyAtmosphere skyAtmosphere() const { return {}; }
    // The sun's linear radiance exactly as the shaders' averSunRadiance() reads it (decoded colour times
    // intensity, 0 in the plain furnace). False where the backend does not know it.
    virtual bool sunRadianceLinear(f32 out[3]) const { out[0] = out[1] = out[2] = 0.0f; return false; }
    // Called after every shader request (compiled or served from the blob cache) with the running totals; null
    // clears it. Process-wide, any thread that compiles. For progress reporting (the editor's shader warm-up).
    using ShaderRequestObserver = void (*)(u32 requests, u32 cacheHits, void* user);
    virtual void setShaderRequestObserver(ShaderRequestObserver, void*) {}
    // Sets the camera post-processing chain.
    virtual void setPostProcess(const PostSettings& p) { (void)p; }
    virtual PostSettings postProcess() const { return {}; }
    // The auto-exposure multiplier CSExposure (post.hlsl) last produced, read back from the GPU a
    // few frames late -- a live UI number. False when auto-exposure hasn't run yet or the backend
    // can't read it back. THE METERED VALUE, before PostSettings::exposure (manual compensation).
    virtual bool postExposureReadout(f32& adaptedExposure) const { (void)adaptedExposure; return false; }
    // Records one draw of `mesh` with a world matrix (row-major), base colour, and PBR
    // metallic/roughness (0..1).
    virtual void drawMesh(MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                          f32 metallic, f32 roughness) {
        (void)mesh; (void)world; (void)baseColor; (void)metallic; (void)roughness;
    }

    // Sets binding table 1 and its b2 constant block, sticky until changed and consumed by every
    // subsequent drawMesh. `constants` is COPIED; the caller may reuse its buffer immediately.
    virtual void setDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) {
        (void)set; (void)constants; (void)bytes;
    }
    // Sets what beginFrame resets the above to. The identity/fallback set belongs here.
    virtual void setDefaultDrawBinding(BindingSetHandle set, const void* constants, u32 bytes) {
        (void)set; (void)constants; (void)bytes;
    }

    // ---- same-frame depth prepass (see docs/RENDERING.md and D3D12Device::drawMesh) ----
    // Off (default) is the unchanged pre-existing path: drawMesh shades immediately.
    // Defaulted no-op so Vulkan's mid-bring-up keeps compiling without this yet.
    virtual void setDepthPrepassEnabled(bool on) { (void)on; }
    virtual bool depthPrepassEnabled() const { return false; }

    // Draws `mesh`'s depth ONLY, via whichever registered feature both overridesScenePipeline() and
    // returns non-zero from depthPrepassPipeline(). `world` matches an equivalent drawMesh call.
    // A CALLER MUST NOT PASS A COMPUTE-WRITTEN (SKINNED) MESH HERE. `color` matches the paired
    // drawMesh()'s array: gBaseColor is the per-object constant that array fills, and the depth
    // shader's alpha test is `gBaseColor.a * gBaseColorFactor.a * texture.a` (voxi.hlsl PSDepthPrepass).
    virtual void drawMeshDepthPrepass(MeshHandle mesh, const f32 world[16], const f32 color[4]) {
        (void)mesh; (void)world; (void)color;
    }

    // drawMeshDepthPrepass's body WITHOUT the frame-wide depthPrepassEnabled() gate or per-frame
    // census -- for a SINGLE draw needing depth written by the alpha-testing depth-only shader
    // immediately before its colour draw, prepass on or off. Follow with setNextDrawPrepassed(true).
    // Exists for an ALPHA-MASKED material under forced early depth: PSMainVoxi is [earlydepthstencil].
    // A COMPUTE-WRITTEN (skinned/soft-body) MESH IS ACCEPTED HERE, unlike drawMeshDepthPrepass.
    // Returns whether depth was written; caller marks the colour draw prepassed only on true.
    virtual bool drawMeshDepthOnly(MeshHandle mesh, const f32 world[16], const f32 color[4]) {
        (void)mesh; (void)world; (void)color; return false;
    }

    // Marks the VERY NEXT drawMesh() call as one whose depth a prior drawMeshDepthPrepass() call
    // already wrote for the identical mesh/world THIS SAME FRAME. Auto-consumed, not sticky.
    // drawMesh always resets it to false, so never calling this reproduces today's behaviour exactly.
    virtual void setNextDrawPrepassed(bool prepassed) { (void)prepassed; }

    // ---- translucency: the blended-mesh path ----
    // Marks every subsequent drawMesh() as TRANSLUCENT until changed. Sticky like setDrawBinding.
    // A blended draw leaves the opaque path entirely, at the top of drawMesh, BEFORE IRenderFeature's
    // submitDraw loop. Ordering: drawn with opaque pass it would land BEFORE the deferred sky, whose
    // depth-EQUAL fill would overwrite it. Cost: excluded from acceleration structure and voxelisation.
    virtual void setDrawBlended(bool blended) { (void)blended; }
    // What setDrawBlended last set, for a caller that saves and restores it around a nested draw.
    virtual bool drawBlended() const { return false; }

    // Unlit line geometry (grid, gizmos, selection outlines, collider/nav overlays): per-vertex
    // DISPLAY colour, drawn as a line list.
    virtual LineHandle createLineMesh(const LineVertex* verts, u32 count) { (void)verts; (void)count; return 0; }
    // EDITOR CHROME, DRAWN AFTER THE CAMERA POST CHAIN: the call QUEUES (mesh, world, state) and
    // the device replays the queue once per frame right after the tonemap, into the display-resolution
    // target the overlay features use. So a line shows exactly its authored colour -- no exposure,
    // tonemap, bloom or local exposure touches it, and AverSR does not resample it -- and is crisp
    // at display resolution. Depth-tested lines (setLineDepth(true), the default) are occluded by
    // sampling the scene depth (sceneDepthTexture) in the pixel shader.
    virtual void drawLines(LineHandle mesh, const f32 world[16]) { (void)mesh; (void)world; }
    // Releases a line mesh's GPU memory. False for a stale or already-released handle.
    // Slot kept, not recycled, same reason as destroyMesh: a stale handle must address a DEAD mesh.
    virtual bool destroyLineMesh(LineHandle mesh) { (void)mesh; return false; }

    // Mesh shader geometry path (mesh-shader Tier 1 + SM 6.5), replacing the input-assembler vertex
    // path for every draw. Ignored when unavailable.
    virtual void setMeshShaders(bool enabled) { (void)enabled; }
    virtual bool meshShadersActive() const { return false; }

    // THE WIREFRAME VIEW, Unreal's: while on, drawMesh shades nothing. Each mesh is queued for the
    // overlay stage and drawn there as unlit edges after the post chain, every edge visible.
    // Sticky until toggled off.
    virtual void setWireframe(bool on) { (void)on; }
    // Draws the next mesh with NO LIGHTING -- flat gBaseColor, no sun, no ambient, no fog.
    // Turns on a path that already existed but was unreachable. Sticky like setWireframe.
    virtual void setUnlit(bool on) { (void)on; }

    // Line depth testing. Default true; false draws subsequent lines as an always-on-top overlay.
    // Sticky; captured per drawLines call.
    virtual void setLineDepth(bool testDepth) { (void)testDepth; }

    // Line thickness in DISPLAY pixels for subsequent drawLines calls. Default 1; sticky like
    // setLineDepth. The caller scales by its own DPI -- the device has no idea what a pixel means.
    virtual void setLineWidth(f32 pixels) { (void)pixels; }

    // Captures the backbuffer pixel at (x,y) during the next presented frame; poll getCapture().
    virtual void requestCapture(u32 x, u32 y) { (void)x; (void)y; }
    virtual bool getCapture(f32 outRGBA[4]) { (void)outRGBA; return false; }
    // Full captured frame (tight RGBA8, top-to-bottom) after a requestCapture completes.
    virtual bool getFrameImage(std::vector<u8>& outRGBA, u32& w, u32& h) { (void)outRGBA; (void)w; (void)h; return false; }

    // Initialises in-window UI on this device. False if the backend has no UI support, or has
    // support but nothing installed to host -- deliberately silent about which toolkit.
    virtual bool uiInit(void* windowHandle) { (void)windowHandle; return false; }
    virtual void uiNewFrame() {}
    virtual void uiShutdown() {}
    virtual bool uiActive() const { return false; }
    virtual bool uiWantsMouse() const { return false; }    // true when the cursor is over UI
    virtual bool uiWantsKeyboard() const { return false; }

    // Makes a texture drawable by the UI, returning the identifier the UI layer expects as a plain
    // integer. Cached on the texture. 0 where the backend hosts no UI.
    virtual u64 uiTextureId(TextureHandle t) { (void)t; return 0; }

    // The backend's OWN scene depth target, registered as an ordinary TextureHandle through the
    // SAME resource-factory table createTexture() populates. Declares its own sample count via
    // sampleCount() above (multisampled needs SlotKind::Texture2DMS, not Texture2D).
    // Defaulted no-op so Vulkan and Null are unaffected. 0 before the first swapchain resize.
    virtual TextureHandle sceneDepthTexture() { return 0; }

    // The opaque scene, copied, so a translucent surface can read what's behind it. Returns a copy
    // of the scene colour taken just BEFORE blended draws replay. One copy, taken once: a second
    // translucent layer samples a background missing the first. 0 when unavailable (before first
    // resize, unimplemented backend, or under MSAA where CopyResource into single-sample is invalid).
    virtual TextureHandle sceneColorBackdropTexture() { return 0; }

    // ---------------------------------------------------------------------------------------
    // G-buffer: velocity, view-space depth, and world normal+roughness, written ALONGSIDE the
    // forward scene pass at scene resolution -- three extra render targets, nothing else changed.
    // Additive and defaulted throughout: never enabling it allocates none of the three targets and
    // renders BIT-IDENTICAL to a build without this declaration.
    virtual void setGBufferEnabled(bool on) { (void)on; }
    virtual bool gBufferEnabled() const { return false; }
    // Whether the three textures below hold this frame's single-sample values for readers. Under MSAA
    // a backend may resolve its multisampled G-buffer into them; one that cannot answers false.
    virtual bool gBufferWritten() const { return gBufferEnabled() && sampleCount() == 1; }

    // Scene-resolution screen-space motion, Format::RG16F. UNITS: TEXELS PER FRAME, DESTINATION
    // TEXEL MINUS SOURCE TEXEL -- for a point shaded at THIS frame's pixel (x,y), stored (vx,vy)
    // satisfies (x,y) - (vx,vy) == where that surface point was LAST frame.
    // 0 when gBufferEnabled() is false or unimplemented.
    virtual TextureHandle gBufferVelocityTexture() { return 0; }

    // Scene-resolution depth, Format::R32Float. UNITS: VIEW-SPACE LINEAR DEPTH (shaded point's Z in
    // view space, i.e. clip-space W pre-divide) -- deliberately NOT sceneDepthTexture()'s post-projection.
    // 0 when gBufferEnabled() is false or unimplemented.
    virtual TextureHandle gBufferViewZTexture() { return 0; }

    // Scene-resolution normal and roughness, Format::RGB10A2Unorm. xy = world-space normal,
    // octahedral-encoded (Cigolle et al. 2014); z = roughness; w = 0.
    // 0 when gBufferEnabled() is false or unimplemented.
    virtual TextureHandle gBufferNormalRoughnessTexture() { return 0; }

    // Previous frame's view-projection -- ROW-MAJOR, ROW-VECTOR, same convention as setCamera's
    // `viewProj` -- valid whenever gBufferEnabled() is true. False when the G-buffer is off or
    // unimplemented, leaving `outPrevViewProj` untouched.
    virtual bool gBufferPrevViewProj(f32 outPrevViewProj[16]) const { (void)outPrevViewProj; return false; }

    // True when the three textures above do NOT describe a continuous previous frame: first frame
    // enabled, a camera cut, a level load, a resolution change, or anything else making reproject
    // against last frame nonsense. Every temporal consumer must ask this, not infer it.
    // Defaults to TRUE (conservative: "assume invalid").
    virtual bool gBufferHistoryInvalid() const { return true; }
};

// Converts a colour temperature in Kelvin to LINEAR sRGB, normalised so the brightest channel is 1.
// Clamped to 1000..15000 K. LINEAR: a caller storing this into a display-encoded field must
// re-encode with pow(x, 1/2.2) first.
void blackbodySrgb(f32 kelvin, f32 outRgb[3]);

// Creates the first available device in the desc's preference order.
IDevice* createDevice(const DeviceDesc& desc = {});
// Destroys a device created by createDevice.
void destroyDevice(IDevice* device);

// Handler a backend hosting ImGui registers, so the platform Window can forward raw messages.
using UiWndProcFn = bool (*)(void* hwnd, u32 msg, u64 wparam, i64 lparam);
// Registers the handler raw window messages are routed to.
void registerUiWndProc(UiWndProcFn fn);
// Forwards one window message to the registered handler. False when there is none.
bool uiWndProc(void* hwnd, u32 msg, u64 wparam, i64 lparam);

} // namespace aver::rhi
