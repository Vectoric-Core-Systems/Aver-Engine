// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Aver RHI â€” the single render-hardware abstraction every GPU consumer targets. Backends
// (D3D12/D3D11/Vulkan) implement these interfaces; a Null backend is always available as a fallback.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/rhi/Atmosphere.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <cstddef>
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

// Interleaved mesh vertex: position + normal (engine space, cm) + UV0. No tangent field; the
// material shading derives its tangent frame from ddx/ddy of world position and UV.
struct MeshVertex {
    f32 px, py, pz;
    f32 nx, ny, nz;
    f32 u, v;
};

// The layout is a cross-file ABI: the D3D12 input layout, HLSL `struct MeshVtx` and the raw root
// SRV stride all name it independently and no compiler checks them.
static_assert(sizeof(MeshVertex) == 32, "MeshVertex is the HLSL MeshVtx / kMeshInputLayout ABI");
static_assert(offsetof(MeshVertex, px) == 0,
              "position must stay first: the DXR BLAS description points at the vertex buffer base");

// Line vertex: position + colour (unlit), for grids/gizmos/debug.
struct LineVertex {
    f32 px, py, pz;
    f32 r, g, b;
};

// How to create a device: backend preference order and development switches.
//
// THE PREFERENCE ORDER IS THE SELECTION MECHANISM, and until now nothing ever wrote it. Every
// caller took the default, so which backend ran was decided entirely by which ones were COMPILED
// IN -- a `#if` in RHI.cpp, not a choice. Principle P7 in ARCHITECTURE.md claims backends are
// "selected at runtime from the compiled-in set", and that was not true of any build.
//
// It matters more than it looks: a second backend that cannot be ASKED FOR cannot be exercised
// either, so it cannot be developed against. Everything written for it would be unreachable code
// that compiles.
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

    // Whether a shader may index a large texture ARRAY by a value it computed -- what texturing a
    // ray hit needs, because one fullscreen pass shades every material and cannot bind a descriptor
    // table per draw the way the raster path does.
    //
    // A SEPARATE BIT FROM resourceBindingTier, NOT DERIVED FROM IT, and that is deliberate on both
    // backends. RHIResources.hpp opens by promising "explicit descriptor tables, NOT bindless",
    // because the engine floor is FL 11_0 (binding tier 1). That promise still holds for every
    // raster pipeline; this bit says only that the RAY-TRACED path, which already demands DXR 1.1
    // and SM 6.5 and therefore hardware that is always binding tier 3, may use one. Deriving it
    // from resourceBindingTier would also be wrong on Vulkan, where that field is hardcoded to 0
    // by design and could never carry this.
    bool rtBindlessTextures = false;
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

// SIMULATED DEVICE LOSS, after this many presented frames. 0 -- the default -- never fires.
//
// DELIBERATELY NOT A CapsOverride TOKEN, even though --force-caps is the obvious neighbour: that
// struct says of itself that every field "only ever reduces a capability", and losing a device is
// not a capability, it is an event. Folding it in there would have made that sentence untrue.
//
// WHY SIMULATE IT AT ALL. Device loss is real -- a driver timeout, a driver update, a hardware
// fault -- and the code that handles it is the code least likely to have ever been run: reaching it
// honestly means making a GPU disappear underneath a live process. So it is the exact shape of path
// that rots silently and is then wrong on the one day it matters. This makes it a command line.
void setSimulatedDeviceLoss(u32 afterPresentedFrames);
u32  simulatedDeviceLoss();

// Camera post-processing: exposure, bloom and eye adaptation.
struct PostSettings {
    // Linear multiplier on scene radiance, applied BEFORE the tonemap. Overridden every frame by
    // the adaptation when autoExposure is on.
    f32 exposure = 1.0f;

    // Bloom. Zero intensity builds no pyramid and records no pass at all.
    f32 bloomIntensity = 0.06f;
    // Luminance above which a pixel contributes, and the width of the soft knee below it.
    f32 bloomThreshold = 1.0f;
    f32 bloomKnee      = 0.5f;

    // Eye adaptation, from a luminance histogram of the frame.
    bool autoExposure   = true;
    f32  exposureMin    = 0.05f;   // clamps on the computed multiplier, not on scene luminance
    f32  exposureMax    = 8.0f;
    f32  exposureSpeed  = 3.0f;    // adaptation rate, in e-folds per second
    f32  exposureKey    = 0.18f;   // middle grey the average luminance is driven towards
    // Fraction of the histogram discarded at each end before averaging.
    f32  histogramLowPercent  = 0.30f;
    f32  histogramHighPercent = 0.85f;
};

// Which sky the engine draws. Authored is a two-colour dome; Physical derives the dome, the direct
// sun's colour and the aerial perspective from Rayleigh/Mie/ozone scattering.
enum class SkyModel : u32 { Authored = 0, Physical = 1 };

// The sky, the sun and the air between them, as a level authors them.
struct SkyAtmosphere {
    bool enabled = false;

    // ---- which model ----
    // Physical is the DEFAULT. The dome, its exponent and the sun's colour below are then derived
    // from the sun's elevation and are inert as authored values; --sky-authored restores them.
    SkyModel          model = SkyModel::Physical;
    AtmosphereProfile air{};   // only read when model is Physical

    // ---- the dome ----
    f32 zenith[3]  = {0.24f, 0.45f, 0.85f};   // authored sRGB, decoded in the shader; Authored only
    f32 horizon[3] = {0.72f, 0.83f, 0.95f};
    f32 atmosphereHeight = 0.65f;   // exponent on the horizon-to-zenith blend
    // What the world below the horizon reflects back into the lower half of the dome.
    f32 groundAlbedo[3] = {0.24f, 0.23f, 0.21f};
    f32 groundBlend     = 1.0f;   // how much of the ground replaces the sky below the horizon
    // Multiplier on the sky-hemisphere irradiance every surface receives; 1.0 is "as bright as the
    // sky actually is".
    f32 skyLightIntensity = 1.0f;

    // ---- the sun ----
    // The authoritative field, pointing TOWARD the light. Need not be normalised. Degrees are the
    // editing form only; setSunAngles / sunAngles convert.
    f32 sunDirection[3] = {-0.5481f, 0.3838f, 0.7431f};
    f32 sunColor[3]     = {1.0f, 0.96f, 0.90f};
    f32 sunIntensity    = 3.0f;   // scales direct light, GI injection and the sun disk alike
    f32 sunTemperatureK = 0.0f;   // Kelvin; 0 means use sunColor as authored
    f32 sunAngularDiameterDeg = 0.545f;   // disk size, and how fast a shadow edge softens

    // WHITE FURNACE radiance. Zero is off, and off is the only value any content should ever use.
    // Non-zero replaces the whole sky, the ground and the sun with a uniform environment of this
    // radiance, which is the standard way to measure whether a shading model conserves energy: a
    // surface of albedo 1 inside it must read exactly this, whatever its orientation and whatever
    // is around it. It lives on the sky rather than in a debug flag because it IS a statement about
    // the environment, and because every path that asks the sky for radiance then gets it for free.
    f32 furnaceRadiance = 0.0f;

    // Keeps the SUN ON inside the furnace, with the uniform environment at zero.
    //
    // A SEPARATE MODE because the two halves of the shading model fail differently and the plain
    // furnace can only see one of them. With the sun off, only the ambient term is exercised -- and
    // that term was already correct when a missing /PI made every SUNLIT reflection 3.14x too
    // bright. The oracle was blind to the exact bug it was built to find.
    //
    // Here an albedo-1 Lambertian surface at angle theta to the sun must have exitant radiance
    // E*cos(theta)/PI, where E is the sun's irradiance. That is an ABSOLUTE claim, and it has to
    // be: a missing 1/PI is a GLOBAL SCALE, so no ratio between two surfaces and no equality
    // between two configurations can see it. Only a comparison against a known number can.
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
    // Which sky this is. Two levels with different seeds get different cloud fields from the same
    // settings, which is what makes a PCGVOLUME's seed reach the sky.
    //
    // ZERO IS THE UNSEEDED FIELD AND REPRODUCES THE PREVIOUS OUTPUT EXACTLY. That is deliberate
    // rather than incidental: it is what lets this land without moving a single recorded gate probe,
    // and what lets anyone bisect a sky change without wondering whether the seed did it.
    i32  cloudSeed     = 0;

    // Writes sunDirection from an elevation above the horizon and an azimuth bearing about +Z
    // from +X, both in degrees.
    void setSunAngles(f32 elevationDeg, f32 azimuthDeg);
    // Reads sunDirection back as elevation and azimuth in degrees.
    void sunAngles(f32& elevationDeg, f32& azimuthDeg) const;
};

// One node of a per-pass GPU timing report -- the public mirror of D3D12Device's private
// GpuAccum tree (see that struct's own comment for why it is a tree keyed by (label, parent)
// rather than a flat list). FLAT AND PARENT-INDEXED, not an owning nested structure: the source
// data is already exactly this shape, so returning it flat is a straight copy, and a caller that
// wants indentation (the console, formatting this the way collectGpuTiming's own log line does)
// walks it with the same O(n) children-list pass collectGpuTiming already builds each report.
//
// `ms` is INCLUSIVE -- itself plus everything nested under it, matching what a begin/end
// timestamp pair actually measures. A caller wanting the pass's OWN time subtracts its direct
// children's `ms`, exactly as collectGpuTiming's Appender does when it prints "(excl ...)".
struct GpuTimingNode {
    // A span index used as "this node is top-level" -- the same sentinel value D3D12Device's
    // private kNoAccumParent uses, so copying GpuAccum into this type needs no remapping pass.
    static constexpr u32 kNoParent = 0xFFFFFFFFu;
    std::string label;
    f64 ms = 0;              // inclusive, averaged across framesAccumulated frames
    u32 parent = kNoParent;  // index into the SAME report's `nodes`, or kNoParent
};

// A snapshot of one device's per-pass GPU timing, as of the last frame it collected one.
//
// TWO INDEPENDENT "NO DATA" AXES, deliberately not collapsed into one empty result:
//   - `supported` is the CAPABILITY axis. False means this backend cannot report timings at all
//     -- Vulkan has no equivalent machinery yet (see its own comment), and a D3D12 device with
//     GPU timing disabled (tsEnabled_ false, e.g. timestamp queries unavailable on this adapter)
//     reports the same way. `nodes` is always empty when this is false.
//   - `nodes` empty (or `framesAccumulated` 0) with `supported` true is the CONTENT axis: an
//     enabled backend that simply has not accumulated a span yet -- frame 0, or every span this
//     frame was dropped (see tsDropped_'s own comment). The console needs to tell "ask again
//     later" apart from "this device will never answer", which is exactly what these two together
//     say and a single bool could not.
struct GpuTimingReport {
    bool supported = false;
    // Averaged over this many frames since boot (see D3D12Device::tsAccumFrames_'s own comment on
    // why an average, not one sampled frame). 0 when nothing has been collected yet.
    u32 framesAccumulated = 0;
    std::vector<GpuTimingNode> nodes;
};

// One GPU device: frame loop, scene state, immediate drawing, capture and in-window UI.
class IDevice {
public:
    virtual ~IDevice() = default;
    virtual Backend backend() const = 0;
    virtual const char* adapterName() const = 0;
    virtual DeviceCaps caps() const { return {}; }

    // Generic resource creation for render-feature modules. nullptr on backends without GPU
    // support, which is how a feature declines to initialise.
    virtual IResourceFactory* resources() { return nullptr; }

    // The SAME command-recording context the backend's own drawMesh() uses internally to let a
    // registered IRenderFeature override individual scene draws (see D3D12Device::drawMesh's
    // overridesScenePipeline branch). Exposed here too so a CALLER -- not just a registered feature
    // -- can interleave its own setPipeline/dispatchMeshClusters-shaped calls with ordinary
    // e.device()->drawMesh() calls in the same frame, for the SUBSET of instances that want a
    // different draw path (per-cluster GPU LOD is the first consumer: most instances still go
    // through drawMesh(), only the ones with cluster data take this). setPipeline() already
    // invalidates the backend's own cached root-signature/PSO state as a side effect (see its own
    // comment), which is what makes drawMesh() calls safe again immediately afterwards -- the same
    // contract IRenderFeature's override path already relies on every frame. nullptr on a backend
    // with no GPU support, exactly like resources().
    virtual IRenderContext* renderContext() { return nullptr; }

    // Render-feature registration. NON-owning: the caller keeps the feature alive.
    virtual void addRenderFeature(IRenderFeature* f) { (void)f; }
    virtual void removeRenderFeature(IRenderFeature* f) { (void)f; }

    // The upscaler that turns the scene-resolution colour into the present-resolution image, or
    // null for none -- which is the default and must stay bit-identical to a build with no upscaler
    // module at all (docs/AVERSR.md's own invariant for quality Off).
    //
    // NON-OWNING, exactly like addRenderFeature above: the caller keeps it alive, and the composition
    // root is the only place that knows a concrete type. That is what lets Aver.Render.Sr be linked
    // by the HOST and by no engine module -- the rule docs/AVERSR.md states and modules/render.voxi
    // already follows.
    //
    // A DEFAULTED NO-OP, so every other IDevice implementation compiles unchanged. The Vulkan
    // backend implements this same interface and is mid-bring-up; a pure virtual here would break
    // its build for a feature it does not yet have.
    virtual void setUpscaler(IUpscaler* u) { (void)u; }
    virtual IUpscaler* upscaler() const { return nullptr; }

    // The SCENE colour target's format, which a backend running a post chain does not present
    // directly. Pipelines drawing into the scene must match it.
    virtual Format backbufferFormat() const { return Format::Unknown; }
    virtual Format depthFormat() const { return Format::Unknown; }

    // Anti-aliasing sample count. Changing it rebuilds the scene targets and every PSO. setter
    // returns false if the count is unsupported.
    virtual u32 sampleCount() const { return 1; }
    virtual bool setSampleCount(u32 samples) { (void)samples; return false; }
    // Creates the swapchain for a native window.
    virtual ISwapchain* createSwapchain(const SwapchainDesc& desc) = 0;
    virtual void beginFrame() = 0; // acquires + clears the current backbuffer
    virtual void endFrame() = 0;   // finalizes the frame's command list

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

    // Decouples the 3D scene's own render targets from the swapchain's: the scene renders at
    // round(present * scale) and the post chain's composite upscales it back to the present size,
    // which the editor UI and the backbuffer/viewport texture stay pinned to (they never see this
    // value). Clamped to [0.25, 1.0]. 1.0 (the default) reproduces the pre-existing behaviour of
    // sizing the scene 1:1 with the swapchain, byte-for-byte -- a backend that never implements this
    // is exactly that default, permanently. Rebuilds the scene-sized targets immediately if a
    // swapchain already exists.
    virtual void setRenderScale(f32 scale) { (void)scale; }
    virtual f32  renderScale() const { return 1.0f; }

    // Sends the post chain's output to an offscreen texture instead of the backbuffer, so the UI
    // can draw the scene as an ordinary image. The texture is the FULL backbuffer size.
    virtual void setViewportToTexture(bool on) { (void)on; }
    virtual bool viewportToTexture() const { return false; }
    // The UI identifier for that texture, or 0 when the mode is off or unsupported.
    virtual u64 viewportTextureId() { return 0; }

    // True once this device has been REMOVED and can no longer execute anything.
    //
    // WHY THIS HAS TO BE ASKABLE. A GPU can be taken away underneath a running process -- a driver
    // timeout, a driver update, a hardware fault -- and the API does NOT report it at the call that
    // caused it. Every later call simply fails, quietly, and the process carries on issuing work
    // into a device that will never run any of it until something finally faults hard. That is the
    // shape of "the engine crashed with no message", and the only way out of it is for the layer
    // that DETECTS the removal to be able to tell the layer that drives the frame.
    //
    // ONE-WAY AND STICKY: nothing here recovers a lost device. Recreating one means recreating every
    // resource every module owns, which is a feature and not an error path; this is the honest
    // minimum, which is to stop, say so, and leave the last good frame on screen.
    //
    // Defaults to false so a backend that cannot lose its device -- and every mock in the tests --
    // is unaffected without writing a line.
    virtual bool deviceLost() const { return false; }

    // TRUE WHEN SOME REGISTERED FEATURE HAS TAKEN THE SCENE OVER -- the path-traced reference view
    // being the one that does today. Exists so a CALLER can decide not to issue a draw at all,
    // which is a different question from the one drawMesh already answers internally.
    //
    // WHY A CALLER NEEDS TO ASK. drawMesh submits to every feature BEFORE it honours suppression,
    // and that order is deliberate: a suppressing feature is usually suppressing precisely because
    // it is building its own scene out of those submissions, so skipping submitDraw would starve it.
    // The consequence is that an EDITOR-ONLY draw -- a selection outline, a gizmo, anything that is
    // chrome rather than scene -- is still handed to a feature that has no way to tell chrome from
    // geometry, and gets baked into its output. Filtering inside drawMesh cannot fix that: by then
    // the information that this draw is chrome is gone.
    //
    // Defaults to false, like deviceLost() above, so a backend that registers no features and every
    // mock in the tests is unaffected without writing a line.
    virtual bool sceneSuppressed() const { return false; }

    // Per-pass GPU timing, for a caller that wants to know where the frame's time went (the
    // command console's frame-time breakdown is the first one) without going through the periodic
    // AVER_INFO log a backend may print on its own.
    //
    // TWO FRAMES OLD, ON PURPOSE. The GPU timestamps this reports were resolved from a readback
    // slice that only becomes readable once the GPU has caught up to it, which beginFrame fences
    // on before collecting -- see D3D12Device's own per-pass-timing comment. Reading "this frame's"
    // own timings would mean blocking on the GPU to ask how fast the GPU was, which would create
    // the very stall it reports. A caller that polls this once a frame is reading a rolling average
    // a couple of frames behind the frame it's currently driving, not a live number.
    //
    // RETURNED BY VALUE, not a reference into backend state: the source data mutates every single
    // beginFrame (new spans folded in, occasionally reallocated), so a reference handed out here
    // would be dangling or stale by the next frame. This is a snapshot -- a dozen or so short-label
    // nodes, cheap to copy -- safe for the caller to hold onto for as long as it likes.
    //
    // Defaults to an unsupported/empty report (GpuTimingReport::supported == false, `nodes` empty)
    // so a backend that has not implemented this -- Vulkan (no equivalent machinery yet), D3D11,
    // Null, and every test mock -- is unaffected without writing a line, same as deviceLost() above.
    virtual GpuTimingReport gpuTiming() const { return {}; }

    // GPU self-test: clears a tiny offscreen target to `in` and reads the pixel back into
    // `outRGBA`. True if the read-back matches.
    virtual bool selfTest(const f32 inRGBA[4], f32 outRGBA[4]) { (void)inRGBA; (void)outRGBA; return false; }

    // Uploads a static mesh (positions+normals+indices). Returns a handle, 0 on failure.
    virtual MeshHandle createMesh(const MeshVertex* verts, u32 vertexCount,
                                  const u32* indices, u32 indexCount) {
        (void)verts; (void)vertexCount; (void)indices; (void)indexCount; return 0;
    }

    // Releases a mesh's GPU memory. False if the handle is invalid, already dead, or still shared.
    //
    // WHY THIS DID NOT EXIST, AND WHY IT HAD TO. Until this, every mesh ever created lived until the
    // device did. Two subsystems hit that and worked around it rather than fixing it:
    // LandscapeRenderer capped its cache and drew a coarser ancestor once full (its own warning said
    // "there is no destroyMesh, so residency cannot be reclaimed"), and SkinnedScene recycles dead
    // entries because it cannot free them. A streaming world makes it fatal rather than untidy:
    // loading chunks in without ever letting them out is a leak with a camera attached.
    //
    // THE HANDLE IS NOT RECYCLED. The slot is cleared and kept, so a stale handle addresses a DEAD
    // mesh and draws nothing. Reusing slots would make it address a DIFFERENT live mesh and silently
    // draw the wrong geometry, which is far harder to notice than a hole. The cost is a few dozen
    // bytes of dead slot per destroyed mesh -- against the megabytes of vertex and index data this
    // actually reclaims. If churn ever makes that matter, the fix is a generation in the handle, not
    // bare reuse.
    //
    // ACCELERATION STRUCTURES GO WITH IT. A BLAS holds the mesh's GPU addresses, so leaving one
    // behind would point ray tracing at freed memory. The backend destroys any it built from this
    // mesh as part of this call.
    virtual bool destroyMesh(MeshHandle mesh) { (void)mesh; return false; }

    // Creates a mesh whose VERTEX BUFFER IS A COMPUTE TARGET, sharing `source`'s index buffer.
    //
    // This is how skinning reaches the rasteriser, and it is deliberately a creation entry point
    // rather than a modifier on a draw. The alternative -- carrying a substitute stream alongside
    // the MeshHandle -- would have to be threaded through IRenderFeature::submitDraw, the feature's
    // own draw record, each of its replay passes and both backend draw verbs, and ONE MISSED SITE
    // IS A REST-POSE SHADOW BESIDE A POSED CHARACTER. Substituting the handle instead leaves every
    // one of those untouched, and gives each instance its own acceleration structure for free,
    // which a shared MeshHandle could never do.
    //
    // The returned buffer is where a compute pass writes rhi::MeshVertex elements. It is SEEDED
    // WITH `source`'S VERTICES, so a mesh drawn before anything has posed it shows the bind pose
    // rather than uninitialised GPU memory -- which would be plausible-looking garbage, not a crash.
    //
    // Zero on failure, and `outVertices` is then untouched.
    virtual MeshHandle createSkinTargetMesh(MeshHandle source, BufferHandle* outVertices) {
        (void)source; (void)outVertices; return 0;
    }

    // Non-zero when a mesh's vertices are WRITTEN BY COMPUTE rather than uploaded once, and the
    // buffer they live in. Zero for every ordinary mesh.
    //
    // This exists so that a consumer which CACHES something derived from the vertices can tell that
    // its cache expires every frame. An acceleration structure is the case that forced it: the
    // renderer memoises one per mesh and never rebuilds it, which is right for static geometry and
    // means a skinned character's ray-traced shadow keeps the silhouette it had when the structure
    // was first built. The character moves; its shadow does not.
    virtual BufferHandle meshVertexBuffer(MeshHandle mesh) const { (void)mesh; return 0; }

    // A mesh's geometry as BUFFERS a shader can be given descriptors over, plus its element counts.
    // False when the backend cannot express it, which is the signal to fall back rather than to
    // read nothing.
    //
    // This is what a RAY needs and a raster draw never did. A hit gives back a primitive index and
    // barycentrics, and nothing else -- reconstructing the position, normal and uv of the triangle
    // that was hit means indexing the mesh's own index and vertex streams from the shader. Until
    // these existed, ray tracing here could answer "is something there" and nothing more, which is
    // exactly as much as a shadow needs and not enough for a reflection.
    virtual bool meshGeometry(MeshHandle mesh, BufferHandle* vb, BufferHandle* ib,
                              u32* vertexCount, u32* indexCount) const {
        (void)mesh; (void)vb; (void)ib; (void)vertexCount; (void)indexCount; return false;
    }
    // A mesh's LOCAL-SPACE bounding sphere -- centre and radius, in the mesh's own coordinate space
    // before any world transform. Computed once at createMesh time from the AABB of its vertices
    // (centre = AABB midpoint, radius = distance to a corner), which is a real, conservative
    // bounding sphere -- it contains the AABB, which contains every vertex -- not a tight one; a
    // caller culling with it can produce a false "might be visible", never a false "definitely is
    // not". False when the backend has no bounds to give, which is the signal to skip culling for
    // that mesh rather than treat an all-zero sphere as a real, radius-zero point.
    virtual bool meshBounds(MeshHandle mesh, f32 outCentre[3], f32* outRadius) const {
        (void)mesh; (void)outCentre; (void)outRadius; return false;
    }
    // Per-frame camera (row-major, row-vector viewProj = view*proj). invViewProj reconstructs
    // world-space rays for the procedural sky.
    virtual void setCamera(const f32 viewProj[16], const f32 invViewProj[16], const f32 cameraPos[3]) {
        (void)viewProj; (void)invViewProj; (void)cameraPos;
    }
    // Reads the camera back, for a feature fitting its own frustum to the view. False when the
    // backend has no camera to give. Any output may be null.
    virtual bool camera(f32 viewProj[16], f32 invViewProj[16], f32 cameraPos[3]) const {
        (void)viewProj; (void)invViewProj; (void)cameraPos; return false;
    }
    // The scene's own viewport rect in target pixels -- {x, y, w, h} -- for a feature reprojecting
    // a screen-space position between frames. NOT necessarily the whole render target: the editor
    // docks the 3D view in a sub-rect of the backbuffer, and NDC alone does not say where that
    // sub-rect sits. False when the backend has no viewport to give.
    virtual bool sceneViewport(f32 rect[4]) const { (void)rect; return false; }
    // Sets the directional light and the ambient term.
    virtual void setLight(const f32 dirToLight[3], const f32 color[3], f32 ambient) { (void)dirToLight; (void)color; (void)ambient; }
    // Sets the sky, the sun and the air. Supersedes setLight for the sun.
    virtual void setSkyAtmosphere(const SkyAtmosphere& s) { (void)s; }
    virtual SkyAtmosphere skyAtmosphere() const { return {}; }
    // Sets the camera post-processing chain.
    virtual void setPostProcess(const PostSettings& p) { (void)p; }
    virtual PostSettings postProcess() const { return {}; }
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
    //
    // OFF (the default) is the entire pre-existing render path, unchanged: drawMesh always shades
    // straight away, drawMeshDepthPrepass is a no-op and setNextDrawPrepassed's flag is never read.
    // A DEFAULTED NO-OP for the same reason setUpscaler is (see its own comment just above): the
    // Vulkan backend is mid-bring-up and must keep compiling without implementing this yet.
    virtual void setDepthPrepassEnabled(bool on) { (void)on; }
    virtual bool depthPrepassEnabled() const { return false; }

    // Draws `mesh`'s depth ONLY, through whichever registered feature both overridesScenePipeline()
    // and returns non-zero from depthPrepassPipeline() -- a no-op otherwise, and a no-op whenever
    // depthPrepassEnabled() is false. `world` is the SAME row-major transform an equivalent drawMesh
    // call would use; base colour/metallic/roughness are not needed here because nothing this draws
    // is shaded, only tested and written.
    //
    // A CALLER MUST NOT OFFER A COMPUTE-WRITTEN (SKINNED) MESH HERE -- see meshVertexBuffer's own
    // comment for what that flag means. This entry point checks it defensively and silently declines
    // rather than trust every future call site to remember, but the caller choosing NOT to call this
    // for such a mesh in the first place is still the primary contract: a skinned entity is one of
    // the paths this feature is meant to exclude, not one it is meant to guard against after the fact.
    virtual void drawMeshDepthPrepass(MeshHandle mesh, const f32 world[16]) { (void)mesh; (void)world; }

    // Marks the VERY NEXT drawMesh() call as one whose depth a prior drawMeshDepthPrepass() call
    // already wrote for the identical mesh/world THIS SAME FRAME, so the backend can ask the scene
    // feature for its depth-tested-only pipeline variant (scenePipeline(..., depthPrepassed=true))
    // instead of the ordinary depth-write one.
    //
    // AUTO-CONSUMED, NOT STICKY -- deliberately unlike setDrawBinding just above. setDrawBinding's
    // material stays correct for every draw until a caller changes it, which is why it is sticky;
    // this flag means something only about the ONE upcoming draw, and dozens of call sites across
    // this codebase (the landscape, skinned characters, editor gizmos, the selection outline) call
    // drawMesh with no idea this flag exists. If it stayed set after being read, the first caller
    // that forgot to clear it would silently hand every draw AFTER it the wrong depth state. Reset to
    // false by drawMesh whether or not it was true, so the default -- never having called this at
    // all -- is exactly today's behaviour everywhere this is not explicitly threaded through.
    virtual void setNextDrawPrepassed(bool prepassed) { (void)prepassed; }

    // ---- translucency: the blended-mesh path ----
    //
    // Marks every subsequent drawMesh() as TRANSLUCENT until a caller says otherwise. STICKY, like
    // setDrawBinding and unlike setNextDrawPrepassed just above, and for the same reason
    // setDrawBinding is: this is a property of the MATERIAL a caller is currently drawing with, not
    // of one upcoming draw. beginFrame resets it to false, so a caller that never touches it gets
    // exactly today's behaviour on every draw.
    //
    // WHAT A TRUE FLAG ACTUALLY CHANGES, because it is more than a blend state. A blended draw
    // leaves the opaque path completely, at the very top of drawMesh, BEFORE the IRenderFeature
    // submitDraw loop -- so a translucent mesh is not voxelised, not put in the ray-tracing
    // acceleration structure, not drawn into the shadow cascade, and not depth-prepassed. The device
    // captures it instead, and replays every captured draw sorted BACK-TO-FRONT after the deferred
    // sky and before IRenderFeature::transparentPass, through
    // scenePipeline(..., blended = true).
    //
    // THE ORDERING IS THE WHOLE POINT AND IT IS NOT NEGOTIABLE. Drawing a blended mesh where the
    // opaque ones are drawn puts it BEFORE the deferred sky, whose own depth-EQUAL opaque fill then
    // overwrites it -- the identical failure the particle pass was moved to fix; see
    // D3D12Device::endFrame's comment at the sky draw. A caller cannot fix this by sorting its own
    // draw list, because the sky is not in that list. Hence the capture.
    //
    // WHAT IT COSTS, stated rather than discovered later: excluded from the acceleration structure
    // means glass casts no ray-traced shadow and does not appear in a reflection; excluded from
    // voxelisation means it contributes no GI bounce. Those are the correct FIRST answers -- a pane
    // of glass that casts a solid black shadow is a worse artefact than one that casts none -- but
    // they are approximations, not physics, and a later pass that wants coloured transmission
    // shadows will have to put translucent draws back into the structure with a material flag the
    // any-hit shader can read.
    virtual void setDrawBlended(bool blended) { (void)blended; }
    // What setDrawBlended last set, for a caller that saves and restores it around a nested draw.
    virtual bool drawBlended() const { return false; }

    // Unlit line geometry (grid, gizmos): per-vertex colour, drawn as a line list.
    virtual LineHandle createLineMesh(const LineVertex* verts, u32 count) { (void)verts; (void)count; return 0; }
    virtual void drawLines(LineHandle mesh, const f32 world[16]) { (void)mesh; (void)world; }
    // Releases a line mesh's GPU memory. False for a stale or already-released handle.
    //
    // WHY THIS HAD TO EXIST. Every line mesh is a committed UPLOAD-heap buffer and there was no way
    // to free one -- createLineMesh appended to a vector that only ever grew. Fine for the handful
    // the editor builds once at startup (the grid, the gizmo, the sculpt ring); a leak per rebuild
    // for anything that CHANGES, which is every debug overlay worth drawing: a navmesh, an agent's
    // current path, a perception cone. docs/CHUNKS.md raised exactly this for meshes (B6) and
    // destroyMesh was added; line meshes were missed.
    //
    // THE SLOT IS KEPT, NOT RECYCLED, exactly as destroyMesh does and for the reason stated there:
    // a stale handle must address a DEAD mesh rather than a live one. Handing handle 3 to a new
    // caller means whoever still held the old 3 starts drawing somebody else's geometry -- a bug
    // that looks like corruption and cannot be traced back to here.
    virtual bool destroyLineMesh(LineHandle mesh) { (void)mesh; return false; }

    // Mesh shader geometry path (mesh-shader Tier 1 + SM 6.5), replacing the input-assembler vertex
    // path for every draw. Ignored when unavailable.
    virtual void setMeshShaders(bool enabled) { (void)enabled; }
    virtual bool meshShadersActive() const { return false; }

    // Renders subsequent meshes as wireframe until toggled off.
    virtual void setWireframe(bool on) { (void)on; }

    // Line depth testing. Default true; false draws subsequent lines as an always-on-top overlay.
    virtual void setLineDepth(bool testDepth) { (void)testDepth; }

    // Captures the backbuffer pixel at (x,y) during the next presented frame; poll getCapture().
    virtual void requestCapture(u32 x, u32 y) { (void)x; (void)y; }
    virtual bool getCapture(f32 outRGBA[4]) { (void)outRGBA; return false; }
    // Full captured frame (tight RGBA8, top-to-bottom) after a requestCapture completes.
    virtual bool getFrameImage(std::vector<u8>& outRGBA, u32& w, u32& h) { (void)outRGBA; (void)w; (void)h; return false; }

    // Initialises in-window UI on this device for a native window. False if the backend has no UI
    // support, or has UI support but nothing was installed for it to host -- see, for the D3D12
    // backend, aver::rhi::d3d12::IUiBackend and installUiBackend (UiBackend.hpp): this interface is
    // deliberately silent about WHICH UI toolkit a backend hosts, Dear ImGui or otherwise. Widgets are
    // built between uiNewFrame() and endFrame().
    virtual bool uiInit(void* windowHandle) { (void)windowHandle; return false; }
    virtual void uiNewFrame() {}
    virtual void uiShutdown() {}
    virtual bool uiActive() const { return false; }
    virtual bool uiWantsMouse() const { return false; }    // true when the cursor is over UI
    virtual bool uiWantsKeyboard() const { return false; }

    // Makes a texture drawable by the UI, returning the identifier the UI layer expects as a plain
    // integer. Cached on the texture. 0 where the backend hosts no UI.
    virtual u64 uiTextureId(TextureHandle t) { (void)t; return 0; }

    // The backend's OWN scene depth target, registered as an ordinary TextureHandle through the SAME
    // resource-factory table createTexture() populates -- so a caller reaches it with the generic
    // setSrv/textureBarrier vocabulary exactly as it would any other texture, rather than this
    // interface growing a bespoke, backend-specific "hand me the raw depth resource" accessor (see
    // modules/occlusion/include/aver/occlusion/Occlusion.hpp's top comment, point (c), for why that
    // distinction was a deliberate design-review correction and not a style preference). Declares
    // ITS OWN sample count as sampleCount() above (a multisampled result needs an SRV slot declared
    // rhi::SlotKind::Texture2DMS, not Texture2D — see that enum's own comment).
    //
    // A DEFAULTED NO-OP, same shape as setUpscaler/setDepthPrepassEnabled above: the Vulkan backend
    // is mid-bring-up and must keep compiling without implementing this yet, and the Null backend has
    // no depth buffer to give at all. 0 before the first swapchain resize has run, exactly like every
    // other size-dependent target this interface exposes.
    virtual TextureHandle sceneDepthTexture() { return 0; }

    // ---------------------------------------------------------------------------------------
    // G-buffer: velocity, view-space depth, and world normal+roughness, written ALONGSIDE the
    // ordinary forward scene pass at scene resolution -- three extra render targets and nothing
    // else, added here rather than replacing anything the scene pass already produces.
    //
    // WHY THIS EXISTS. Aver is a FORWARD renderer: PSMainVoxi returns a single SV_TARGET, so a
    // shaded pixel's normal, roughness and depth exist only in that one shader invocation's own
    // registers and are gone the instant it returns -- never in a texture any LATER pass, compute
    // or otherwise, can read. That single gap blocks, simultaneously: the vendored FidelityFX
    // Denoiser (third_party/fidelityfx-denoiser -- its README lists FFX_DNSR_Shadows_ReadDepth /
    // ReadNormals / ReadVelocity / ReadPreviousDepth as callbacks the HOST must supply, and today
    // the honest answer to each is "no"), FSR2/3, TAA, and screen-space reflections -- every one of
    // them is a compute pass that needs to read what a previous pass saw, and until now nothing
    // could hand it that. It is also why today's temporal reprojection is WRONG for a moving
    // object: it reprojects THIS frame's world position through LAST frame's camera (gPrevViewProj
    // in VoxiShaders.hpp), which is only correct for geometry that did not move between frames --
    // fixing that fully needs a per-instance previous transform this G-buffer does not carry (see
    // RtInstance's own comment in VoxiRenderer.hpp), but the per-pixel velocity below is the piece
    // every consumer in the list above needs regardless, and it is what this declares.
    //
    // See docs/rendering/DENOISING.md for the fuller design writeup this slice is drawn from.
    //
    // ADDITIVE AND DEFAULTED, THE WHOLE WAY DOWN. setGBufferEnabled defaults to OFF, and every
    // accessor below defaults to its own "nothing here" value (0 for a texture, false/true for the
    // bools, chosen as whichever is the SAFE reading for a caller that forgot to check
    // gBufferEnabled() first). A build that never calls setGBufferEnabled(true) -- which is every
    // build today, since nothing yet does -- allocates none of these three targets, records no
    // extra writes, and renders a frame BIT-IDENTICAL to one from before this declaration existed.
    // That is not a nicety: a render-gate oracle (18 gates x 9 configurations) and 89 headless
    // suites both assume it, and a backend that allocates or writes any of this while reporting
    // gBufferEnabled() == false would fail both without the failure pointing at why.
    //
    // WHO IMPLEMENTS WHAT. This class only declares the surface. Deciding how the three targets are
    // populated -- which pass writes them, whether that is a same-frame prepass or folded into the
    // existing forward pass's pixel shader, how a Vulkan backend mid-bring-up stages the work -- is
    // the backend's call, not this header's; every method here is a virtual with an inert default
    // specifically so a backend that has not done that work yet keeps compiling and keeps behaving
    // exactly as it does today.
    virtual void setGBufferEnabled(bool on) { (void)on; }
    virtual bool gBufferEnabled() const { return false; }

    // Scene-resolution screen-space motion, Format::RG16F. UNITS: TEXELS PER FRAME, DESTINATION
    // TEXEL MINUS SOURCE TEXEL -- for a surface point shaded at THIS frame's pixel (x, y), the
    // stored (vx, vy) satisfies (x, y) - (vx, vy) == the pixel that SAME surface point occupied
    // LAST frame. This is deliberately the identical convention UpscalerNeeds::MotionVectors
    // already documents (RHIResources.hpp): FSR2/3, DLSS, and FFX_DNSR_Shadows_ReadVelocity all
    // read texel-space motion in this direction, and a G-buffer that disagreed with the upscaler
    // seam's own documented meaning would make "motion vectors" mean two different things
    // depending on which consumer asked -- exactly the kind of unit mismatch that never shows up
    // as a compile error or a crash, only as history that reprojects to the wrong pixel.
    //
    // 0 when gBufferEnabled() is false or this backend has not implemented the G-buffer -- the same
    // "absent, not garbage" contract sceneDepthTexture() above already uses. A caller must check
    // gBufferEnabled() (or simply treat a 0 handle as "nothing to read") rather than assume a
    // non-zero handle whenever the G-buffer is on; see this method family's own top comment for why
    // every accessor here defaults to an unambiguous "nothing here" value.
    virtual TextureHandle gBufferVelocityTexture() { return 0; }

    // Scene-resolution depth, Format::R32Float. UNITS: VIEW-SPACE LINEAR DEPTH -- the shaded
    // point's Z in camera/view space, equivalently clip-space W before the perspective divide --
    // and DELIBERATELY NOT the post-projection [0,1] value sceneDepthTexture()'s own depth-stencil
    // buffer holds. Those two quantities are related by a projection-dependent, NON-LINEAR
    // remapping (the classic "depth precision" curve), so a consumer that samples this texture and
    // treats it as ordinary [0,1] depth gets a number wrong by a different factor at every pixel --
    // not a crash, not an obviously bad image, just quietly incorrect reprojection and reconstructed
    // world positions. FFX_DNSR_Shadows_ReadDepth / ReadPreviousDepth and any future SSR pass both
    // want THIS linear form because it is what makes reconstructing a view-space position from a
    // screen UV a single division rather than an un-projection through the full projection matrix.
    //
    // 0 when gBufferEnabled() is false or unimplemented, matching gBufferVelocityTexture() above.
    virtual TextureHandle gBufferViewZTexture() { return 0; }

    // Scene-resolution normal and roughness, Format::RGB10A2Unorm. xyz: the shaded surface's
    // WORLD-SPACE (not view-space, not tangent-space) normal, ENCODED from its real range of
    // [-1, 1] into the unorm-storable range [0, 1] via n*0.5 + 0.5 -- a reader must decode with
    // n*2 - 1 before using it as a direction, and a reader that forgets the decode gets a vector
    // that LOOKS plausible (still roughly unit-length-ish, still roughly pointing outward) while
    // being wrong at every pixel, which is exactly the shape of bug that survives a casual visual
    // check. w: perceptual roughness, already [0, 1], stored as-is with no further transform -- it
    // gets only 2 bits of the format's 10/10/10/2 split, which is deliberately coarse: this channel
    // is read by a denoiser's edge-stopping weight (FFX_DNSR_Shadows_ReadNormals reads roughness
    // alongside the normal for exactly that), never by anything doing actual PBR shading with it,
    // so 2-bit banding here costs nothing a consumer of this field would notice.
    //
    // 0 when gBufferEnabled() is false or unimplemented, matching the two accessors above.
    virtual TextureHandle gBufferNormalRoughnessTexture() { return 0; }

    // Previous frame's view-projection -- ROW-MAJOR, ROW-VECTOR, the SAME convention setCamera's
    // own `viewProj` argument uses above -- valid whenever gBufferEnabled() is true. EVERY temporal
    // consumer of the three textures above needs this: each is a snapshot of ONE frame, and
    // reprojecting that snapshot against accumulated history (FFX_DNSR_Shadows_
    // GetReprojectionMatrix's own job, and this method's direct answer to it) is meaningless
    // without knowing what camera the frame being reprojected FROM was rendered with. False when
    // the G-buffer is off or this backend has not implemented it, leaving `outPrevViewProj`
    // untouched -- the same "ask before you trust it" contract camera() above already uses, and for
    // the same reason: a caller that skips the check and reads untouched memory as a matrix will
    // not get a crash, it will get a plausible-looking wrong reprojection.
    virtual bool gBufferPrevViewProj(f32 outPrevViewProj[16]) const { (void)outPrevViewProj; return false; }

    // True when the three textures above -- and gBufferPrevViewProj, which is meaningless without
    // them -- do NOT describe a continuous previous frame: the first frame the G-buffer was
    // enabled, a camera cut, a level load, a change of render resolution, or anything else that
    // makes "reproject against last frame" produce nonsense rather than merely one frame of stale
    // data. EVERY temporal consumer must ask this rather than infer it, because getting it wrong is
    // not a crash and not even reliably visible in isolation -- it is ONE BAD FRAME immediately
    // after every cut, exactly the shape of defect a single still-frame screenshot review never
    // catches and a person actually moving the camera always will (this engine's own broken
    // per-object reprojection, above, is a standing example of a temporal bug nobody noticed until
    // something moved).
    //
    // Defaults to TRUE -- the conservative answer, "assume history is invalid" -- so a caller that
    // correctly checks this before trusting history, but talks to a backend that has not
    // implemented the G-buffer at all, drops one frame of temporal reuse rather than silently
    // accumulating against a previous frame that was never actually rendered into these targets.
    virtual bool gBufferHistoryInvalid() const { return true; }
};

// Converts a colour temperature in Kelvin to LINEAR sRGB, normalised so the brightest channel is 1.
// Clamped to 1000..15000 K.
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
